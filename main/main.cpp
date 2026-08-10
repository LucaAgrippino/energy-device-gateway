#include <memory>

#include "ImuTask.hpp"
#include "Mpu9150.hpp"
#include "Reading.hpp"
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

std::unique_ptr<Mpu9150> g_imu;
ImuTaskContext g_imu_ctx;
}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "energy-device-gateway starting");

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

    // One mailbox per axis: ImuReading's seven fields don't fit a single
    // Reading, so imuTask fans each poll out across seven mailboxes.
    g_imu_ctx.imu = g_imu.get();
    g_imu_ctx.accel_x_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.accel_y_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.accel_z_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_x_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_y_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.gyro_z_mailbox = xQueueCreate(1, sizeof(Reading));
    g_imu_ctx.temp_mailbox = xQueueCreate(1, sizeof(Reading));

    xTaskCreate(imuTask, "imu_task", kImuTaskStackBytes, &g_imu_ctx, kImuTaskPriority, nullptr);
}
