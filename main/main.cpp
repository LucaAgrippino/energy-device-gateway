#include <memory>
#include <vector>

#include "Aggregator.hpp"
#include "ImuTask.hpp"
#include "Mpu9150.hpp"
#include "Reading.hpp"
#include "WifiManager.hpp"
#include "WsPublisher.hpp"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace {
constexpr const char* kTag = "main";

constexpr gpio_num_t kImuSdaPin = GPIO_NUM_1;
constexpr gpio_num_t kImuSclPin = GPIO_NUM_2;
constexpr uint8_t kImuAddr = 0x69;

// DESIGN.md §6 "Task Parameters".
constexpr uint32_t kImuTaskStackBytes = 4096;
constexpr UBaseType_t kImuTaskPriority = 5;

// publisher DESIGN.md §7 "Task Parameters".
constexpr uint32_t kPublisherTaskStackBytes = 6144;
constexpr UBaseType_t kPublisherTaskPriority = 2;

// aggregator DESIGN.md §5 "Task Parameters".
constexpr uint32_t kAggregatorTaskStackBytes = 4096;
constexpr UBaseType_t kAggregatorTaskPriority = 3;

// Both publisher and aggregator DESIGN.md task tables specify "Core 1 — keep
// off core 0 (Wi-Fi)"; plain xTaskCreate doesn't pin, so both use
// xTaskCreatePinnedToCore instead.
constexpr BaseType_t kSensorPipelineCore = 1;

std::unique_ptr<Mpu9150> g_imu;
ImuTaskContext g_imu_ctx;
std::unique_ptr<WifiManager> g_wifi;
std::unique_ptr<WsPublisher> g_publisher;
std::unique_ptr<Aggregator> g_aggregator;

void publisherTaskFn(void* param) {
    static_cast<WsPublisher*>(param)->run();
}

void aggregatorTaskFn(void* param) {
    static_cast<Aggregator*>(param)->run();
}
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "energy-device-gateway starting");

    g_wifi = std::make_unique<WifiManager>();
    ESP_ERROR_CHECK(g_wifi->init());

    // wifi_manager DESIGN.md §6 "Boot Sequence with NVS": credentials found ->
    // STA, otherwise fall back to AP (provisioning mode). Bring-up failure is
    // logged rather than fatal — sensors should keep running without network,
    // same resilience principle as publisher DESIGN.md §10.
    char ssid[33];
    char password[65];
    esp_err_t wifi_err = ESP_OK;
    if (g_wifi->loadCredentials(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        wifi_err = g_wifi->startSta(ssid, password);
    } else {
        wifi_err = g_wifi->startAp();
    }
    if (wifi_err != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi bring-up failed: %s — continuing without network",
                 esp_err_to_name(wifi_err));
    }

    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = kImuSdaPin;
    bus_cfg.scl_io_num = kImuSclPin;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;

    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    g_imu = std::make_unique<Mpu9150>(bus, kImuAddr);
    ESP_ERROR_CHECK(g_imu->init());

    g_imu_ctx.imu = g_imu.get();
    g_imu_ctx.accel_x_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.accel_y_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.accel_z_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_x_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_y_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_z_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.temp_mailbox = xQueueCreate(1, sizeof(Reading));

    xTaskCreate(imuTask, "imu_task", kImuTaskStackBytes, &g_imu_ctx, kImuTaskPriority, nullptr);

    // Modbus RTU/TCP mailboxes: no producer task exists yet (modbus_device,
    // modbus_tcp aren't built), so these stay empty. That's a normal,
    // documented state per aggregator DESIGN.md §7 — an empty mailbox reports
    // Status::TIMEOUT rather than blocking or erroring.
    QueueHandle_t rtu_mailbox = xQueueCreate(1, sizeof(Reading));
    QueueHandle_t tcp_mailbox = xQueueCreate(1, sizeof(Reading));

    g_publisher = std::make_unique<WsPublisher>();
    esp_err_t publisher_err = g_publisher->start();
    if (publisher_err != ESP_OK) {
        // publisher DESIGN.md §10: publisher failing shouldn't stop sensors.
        ESP_LOGE(kTag, "publisher start failed: %s — dashboard will be unreachable",
                 esp_err_to_name(publisher_err));
    }
    xTaskCreatePinnedToCore(publisherTaskFn, "publisher_task", kPublisherTaskStackBytes,
                            g_publisher.get(), kPublisherTaskPriority, nullptr, kSensorPipelineCore);

    constexpr int64_t kImuStaleUs = CONFIG_IMU_STALE_TIMEOUT_MS * 1000LL;
    std::vector<MailboxEntry> mailboxes = {
        {.queue = g_imu_ctx.accel_x_mailbox, .name = "imu.accel_x", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.accel_y_mailbox, .name = "imu.accel_y", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.accel_z_mailbox, .name = "imu.accel_z", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_x_mailbox, .name = "imu.gyro_x", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_y_mailbox, .name = "imu.gyro_y", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_z_mailbox, .name = "imu.gyro_z", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.temp_mailbox, .name = "imu.temp", .timeout_us = kImuStaleUs},
        {.queue = rtu_mailbox,
         .name = "modbus_rtu",
         .timeout_us = CONFIG_MODBUS_RTU_STALE_TIMEOUT_MS * 1000LL},
        {.queue = tcp_mailbox,
         .name = "modbus_tcp",
         .timeout_us = CONFIG_MODBUS_TCP_STALE_TIMEOUT_MS * 1000LL},
    };

    g_aggregator = std::make_unique<Aggregator>(std::move(mailboxes), *g_publisher);
    xTaskCreatePinnedToCore(aggregatorTaskFn, "aggregator_task", kAggregatorTaskStackBytes,
                            g_aggregator.get(), kAggregatorTaskPriority, nullptr, kSensorPipelineCore);
}
