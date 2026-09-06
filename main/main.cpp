#include <memory>
#include <vector>

#include "Aggregator.hpp"
#include "HealthMonitor.hpp"
#include "ImuTask.hpp"
#include "ModbusRtuDevice.hpp"
#include "ModbusRtuTask.hpp"
#include "ModbusTcpDevice.hpp"
#include "ModbusTcpTask.hpp"
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

// modbus_device DESIGN.md §10 "Task Parameters".
constexpr uint32_t kModbusRtuTaskStackBytes = 4096;
constexpr UBaseType_t kModbusRtuTaskPriority = 4;

// modbus_device DESIGN_TCP.md §6 "Task Parameters".
constexpr uint32_t kModbusTcpTaskStackBytes = 4096;
constexpr UBaseType_t kModbusTcpTaskPriority = 4;

// health DESIGN.md §7 "Task Parameters" — lowest priority, no core affinity,
// so monitoring never preempts the sensor pipeline.
constexpr uint32_t kHealthTaskStackBytes = 4096;
constexpr UBaseType_t kHealthTaskPriority = 1;

// Both publisher and aggregator DESIGN.md task tables specify "Core 1 — keep
// off core 0 (Wi-Fi)"; plain xTaskCreate doesn't pin, so both use
// xTaskCreatePinnedToCore instead.
constexpr BaseType_t kSensorPipelineCore = 1;

std::unique_ptr<Mpu9150> g_imu;
ImuTaskContext g_imu_ctx;
std::unique_ptr<WifiManager> g_wifi;
std::unique_ptr<WsPublisher> g_publisher;
std::unique_ptr<Aggregator> g_aggregator;
std::unique_ptr<ModbusRtuDevice> g_modbus_rtu;
ModbusRtuTaskContext g_modbus_rtu_ctx;
std::unique_ptr<ModbusTcpDevice> g_modbus_tcp;
ModbusTcpTaskContext g_modbus_tcp_ctx;
std::unique_ptr<HealthMonitor> g_health;

// health DESIGN.md §3: app_main creates every task and hands their handles to
// HealthMonitor, rather than the monitor discovering tasks at runtime. A task
// that failed to start stays null and is skipped (§9).
TaskHandle_t g_imu_task{nullptr};
TaskHandle_t g_modbus_rtu_task{nullptr};
TaskHandle_t g_modbus_tcp_task{nullptr};
TaskHandle_t g_publisher_task{nullptr};
TaskHandle_t g_aggregator_task{nullptr};
TaskHandle_t g_health_task{nullptr};

// modbus_device DESIGN.md §6, using the simulated-inverter map from
// VISION.md §7.2. Names are fully qualified ("modbus_rtu.voltage") to match the
// source-naming convention documented in Reading.hpp, which the dashboard and
// the aggregator's MailboxEntry list both key off.
const std::vector<RegisterDef> kInverterRegisters = {
    {.address = 0, .name = "modbus_rtu.voltage", .scale = 0.1f, .reg_count = 1},
    {.address = 1, .name = "modbus_rtu.current", .scale = 0.01f, .reg_count = 1},
    {.address = 2, .name = "modbus_rtu.power", .scale = 1.0f, .reg_count = 1},
    // Registers 3-4 hold one uint32, big-endian, ×0.1 → kWh.
    {.address = 3, .name = "modbus_rtu.energy_total", .scale = 0.1f, .reg_count = 2},
    {.address = 5, .name = "modbus_rtu.status", .scale = 1.0f, .reg_count = 1},
};

// Same simulated inverter, reached over Wi-Fi instead of RS-485, so the map is
// identical apart from the source prefix the dashboard keys off.
const std::vector<RegisterDef> kInverterRegistersTcp = {
    {.address = 0, .name = "modbus_tcp.voltage", .scale = 0.1f, .reg_count = 1},
    {.address = 1, .name = "modbus_tcp.current", .scale = 0.01f, .reg_count = 1},
    {.address = 2, .name = "modbus_tcp.power", .scale = 1.0f, .reg_count = 1},
    {.address = 3, .name = "modbus_tcp.energy_total", .scale = 0.1f, .reg_count = 2},
    {.address = 5, .name = "modbus_tcp.status", .scale = 1.0f, .reg_count = 1},
};

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

    xTaskCreate(imuTask, "imu_task", kImuTaskStackBytes, &g_imu_ctx, kImuTaskPriority,
                &g_imu_task);

    // One mailbox per RTU register, in register-map order (modbus_device
    // DESIGN.md §10, fanned out the same way as the IMU rather than shipping a
    // std::vector through a queue).
    for (size_t i = 0; i < kInverterRegisters.size(); i++) {
        g_modbus_rtu_ctx.mailboxes.push_back(xQueueCreate(1, sizeof(Reading)));
    }

    g_modbus_rtu = std::make_unique<ModbusRtuDevice>(
        static_cast<uart_port_t>(CONFIG_MODBUS_RTU_UART_PORT),
        static_cast<uint8_t>(CONFIG_MODBUS_RTU_SLAVE_ADDR),
        static_cast<uint32_t>(CONFIG_MODBUS_RTU_BAUD_RATE),
        static_cast<gpio_num_t>(CONFIG_MODBUS_RTU_TX_PIN),
        static_cast<gpio_num_t>(CONFIG_MODBUS_RTU_RX_PIN),
        static_cast<gpio_num_t>(CONFIG_MODBUS_RTU_DE_RE_PIN),
        kInverterRegisters);

    // DESIGN.md §12 calls a UART init failure fatal. It is handled here the way
    // Wi-Fi and the publisher already are instead: log it and carry on, so a
    // missing RS-485 adapter degrades to Status::TIMEOUT on those registers
    // (aggregator DESIGN.md §7) rather than reboot-looping the whole gateway
    // and taking the working IMU pipeline down with it.
    esp_err_t rtu_err = g_modbus_rtu->init();
    if (rtu_err == ESP_OK) {
        g_modbus_rtu_ctx.device = g_modbus_rtu.get();
        xTaskCreatePinnedToCore(modbusRtuTask, "modbus_rtu_task", kModbusRtuTaskStackBytes,
                                &g_modbus_rtu_ctx, kModbusRtuTaskPriority, &g_modbus_rtu_task,
                                kSensorPipelineCore);
    } else {
        ESP_LOGE(kTag, "Modbus RTU init failed: %s — those registers stay stale",
                 esp_err_to_name(rtu_err));
    }

    // One mailbox per TCP register, same fan-out as RTU.
    for (size_t i = 0; i < kInverterRegistersTcp.size(); i++) {
        g_modbus_tcp_ctx.mailboxes.push_back(xQueueCreate(1, sizeof(Reading)));
    }

    g_modbus_tcp = std::make_unique<ModbusTcpDevice>(
        CONFIG_MODBUS_TCP_IP,
        static_cast<uint16_t>(CONFIG_MODBUS_TCP_PORT),
        static_cast<uint8_t>(CONFIG_MODBUS_TCP_UNIT_ID),
        kInverterRegistersTcp);

    // The task blocks on the Wi-Fi event group before touching a socket, so it
    // is safe to start here even though the station may not be up yet.
    g_modbus_tcp_ctx.device = g_modbus_tcp.get();
    g_modbus_tcp_ctx.wifi_event_group = g_wifi->eventGroup();
    g_modbus_tcp_ctx.connected_bit = WifiManager::CONNECTED_BIT;
    xTaskCreatePinnedToCore(modbusTcpTask, "modbus_tcp_task", kModbusTcpTaskStackBytes,
                            &g_modbus_tcp_ctx, kModbusTcpTaskPriority, &g_modbus_tcp_task,
                            kSensorPipelineCore);

    g_publisher = std::make_unique<WsPublisher>();
    esp_err_t publisher_err = g_publisher->start();
    if (publisher_err != ESP_OK) {
        // publisher DESIGN.md §10: publisher failing shouldn't stop sensors.
        ESP_LOGE(kTag, "publisher start failed: %s — dashboard will be unreachable",
                 esp_err_to_name(publisher_err));
    }
    xTaskCreatePinnedToCore(publisherTaskFn, "publisher_task", kPublisherTaskStackBytes,
                            g_publisher.get(), kPublisherTaskPriority, &g_publisher_task,
                            kSensorPipelineCore);

    constexpr int64_t kImuStaleUs = CONFIG_IMU_STALE_TIMEOUT_MS * 1000LL;
    std::vector<MailboxEntry> mailboxes = {
        {.queue = g_imu_ctx.accel_x_mailbox, .name = "imu.accel_x", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.accel_y_mailbox, .name = "imu.accel_y", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.accel_z_mailbox, .name = "imu.accel_z", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_x_mailbox, .name = "imu.gyro_x", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_y_mailbox, .name = "imu.gyro_y", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.gyro_z_mailbox, .name = "imu.gyro_z", .timeout_us = kImuStaleUs},
        {.queue = g_imu_ctx.temp_mailbox, .name = "imu.temp", .timeout_us = kImuStaleUs},
    };

    // Append one aggregator entry per RTU register, keeping the mailbox order
    // and the register-map order aligned.
    for (size_t i = 0; i < kInverterRegisters.size(); i++) {
        mailboxes.push_back({.queue = g_modbus_rtu_ctx.mailboxes[i],
                             .name = kInverterRegisters[i].name,
                             .timeout_us = CONFIG_MODBUS_RTU_STALE_TIMEOUT_MS * 1000LL});
    }
    for (size_t i = 0; i < kInverterRegistersTcp.size(); i++) {
        mailboxes.push_back({.queue = g_modbus_tcp_ctx.mailboxes[i],
                             .name = kInverterRegistersTcp[i].name,
                             .timeout_us = CONFIG_MODBUS_TCP_STALE_TIMEOUT_MS * 1000LL});
    }

    g_aggregator = std::make_unique<Aggregator>(std::move(mailboxes), *g_publisher);
    xTaskCreatePinnedToCore(aggregatorTaskFn, "aggregator_task", kAggregatorTaskStackBytes,
                            g_aggregator.get(), kAggregatorTaskPriority, &g_aggregator_task,
                            kSensorPipelineCore);

    // health DESIGN.md §3: the handler is registered on the publisher's server,
    // whose lifetime the publisher owns — so this must come after start(), and
    // is skipped entirely if the server never came up.
    g_health = std::make_unique<HealthMonitor>(
        g_publisher->serverHandle(),
        std::vector<TaskInfo>{
            {.name = "imu", .handle = g_imu_task, .stack_total = kImuTaskStackBytes},
            {.name = "modbus_rtu", .handle = g_modbus_rtu_task,
             .stack_total = kModbusRtuTaskStackBytes},
            {.name = "modbus_tcp", .handle = g_modbus_tcp_task,
             .stack_total = kModbusTcpTaskStackBytes},
            {.name = "aggregator", .handle = g_aggregator_task,
             .stack_total = kAggregatorTaskStackBytes},
            {.name = "publisher", .handle = g_publisher_task,
             .stack_total = kPublisherTaskStackBytes},
            // Handle filled in immediately after xTaskCreate below — the task
            // does not exist yet, and a null handle is skipped until it does.
            {.name = "health", .handle = nullptr, .stack_total = kHealthTaskStackBytes},
        });

    esp_err_t health_err = g_health->init();
    if (health_err != ESP_OK) {
        ESP_LOGW(kTag, "health endpoint unavailable: %s", esp_err_to_name(health_err));
    }
    xTaskCreate(healthTask, "health_task", kHealthTaskStackBytes, g_health.get(),
                kHealthTaskPriority, &g_health_task);
    g_health->setTaskHandle("health", g_health_task);
}
