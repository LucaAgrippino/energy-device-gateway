#include "Mpu9150.hpp"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr const char* kTag = "Mpu9150";

constexpr uint8_t kRegPwrMgmt1 = 0x6B;
constexpr uint8_t kRegWhoAmI = 0x75;
constexpr uint8_t kExpectedWhoAmI = 0x68;
constexpr uint8_t kRegAccelXoutH = 0x3B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegGyroConfig = 0x1B;

constexpr int kI2cTimeoutMs = 1000;
constexpr uint32_t kI2cClockHz = 400000;

constexpr float kGravityMs2 = 9.80665f;
constexpr float kAccelSensitivityLsbPerG = 16384.0f;   // ±2g range
constexpr float kGyroSensitivityLsbPerDegPerS = 131.0f; // ±250°/s range
constexpr float kTempSensitivityLsbPerC = 340.0f;
constexpr float kTempOffsetC = 36.53f;

int16_t toI16(uint8_t hi, uint8_t lo) {
    return static_cast<int16_t>((static_cast<uint16_t>(hi) << 8) | lo);
}
}  // namespace

Mpu9150::Mpu9150(i2c_master_bus_handle_t bus, uint8_t addr)
    : bus_(bus), addr_(addr) {
    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr_;
    dev_cfg.scl_speed_hz = kI2cClockHz;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_, &dev_cfg, &dev_handle_));
}

Mpu9150::~Mpu9150() {
    if (dev_handle_ != nullptr) {
        esp_err_t err = i2c_master_bus_rm_device(dev_handle_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "failed to remove I2C device: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t Mpu9150::init() {
    esp_err_t err = writeReg(kRegPwrMgmt1, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to wake device: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    uint8_t who_am_i = 0;
    err = readRegs(kRegWhoAmI, &who_am_i, 1);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to read WHO_AM_I: %s", esp_err_to_name(err));
        return err;
    }
    if (who_am_i != kExpectedWhoAmI) {
        ESP_LOGE(kTag, "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X", who_am_i, kExpectedWhoAmI);
        return ESP_ERR_NOT_FOUND;
    }

    err = writeReg(kRegAccelConfig, 0x00);  // ±2g
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure accel range: %s", esp_err_to_name(err));
        return err;
    }

    err = writeReg(kRegGyroConfig, 0x00);  // ±250°/s
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to configure gyro range: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "%.*s initialized (addr 0x%02X)",
             static_cast<int>(name().size()), name().data(), addr_);
    return ESP_OK;
}

ImuReading Mpu9150::read() {
    uint8_t buf[14];
    esp_err_t err = readRegs(kRegAccelXoutH, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "burst read failed: %s, returning last known-good reading",
                 esp_err_to_name(err));
        return snapshotLastReading();
    }

    int16_t raw_accel_x = toI16(buf[0], buf[1]);
    int16_t raw_accel_y = toI16(buf[2], buf[3]);
    int16_t raw_accel_z = toI16(buf[4], buf[5]);
    int16_t raw_temp = toI16(buf[6], buf[7]);
    int16_t raw_gyro_x = toI16(buf[8], buf[9]);
    int16_t raw_gyro_y = toI16(buf[10], buf[11]);
    int16_t raw_gyro_z = toI16(buf[12], buf[13]);

    last_reading_.accel_x = scaleAccel(raw_accel_x);
    last_reading_.accel_y = scaleAccel(raw_accel_y);
    last_reading_.accel_z = scaleAccel(raw_accel_z);
    last_reading_.gyro_x = scaleGyro(raw_gyro_x);
    last_reading_.gyro_y = scaleGyro(raw_gyro_y);
    last_reading_.gyro_z = scaleGyro(raw_gyro_z);
    last_reading_.temperature = scaleTemp(raw_temp);
    last_reading_.timestamp = esp_timer_get_time();

    return snapshotLastReading();
}

float Mpu9150::scaleAccel(int16_t raw) {
    return (static_cast<float>(raw) / kAccelSensitivityLsbPerG) * kGravityMs2;
}

float Mpu9150::scaleGyro(int16_t raw) {
    return static_cast<float>(raw) / kGyroSensitivityLsbPerDegPerS;
}

float Mpu9150::scaleTemp(int16_t raw) {
    return (static_cast<float>(raw) / kTempSensitivityLsbPerC) + kTempOffsetC;
}

esp_err_t Mpu9150::writeReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev_handle_, buf, sizeof(buf), kI2cTimeoutMs);
}

esp_err_t Mpu9150::readRegs(uint8_t start_reg, uint8_t* buf, size_t len) {
    return i2c_master_transmit_receive(dev_handle_, &start_reg, 1, buf, len, kI2cTimeoutMs);
}

ImuReading Mpu9150::snapshotLastReading() const {
    ImuReading out;
    out.accel_x = last_reading_.accel_x;
    out.accel_y = last_reading_.accel_y;
    out.accel_z = last_reading_.accel_z;
    out.gyro_x = last_reading_.gyro_x;
    out.gyro_y = last_reading_.gyro_y;
    out.gyro_z = last_reading_.gyro_z;
    out.temperature = last_reading_.temperature;
    out.timestamp = last_reading_.timestamp;
    return out;
}
