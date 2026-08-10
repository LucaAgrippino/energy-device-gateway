#pragma once

#include "IImu.hpp"
#include "driver/i2c_master.h"

class Mpu9150 : public IImu {
public:
    Mpu9150(i2c_master_bus_handle_t bus, uint8_t addr = 0x69);
    ~Mpu9150() override;

    // Non-copyable, non-movable (RAII resource holder)
    Mpu9150(const Mpu9150&) = delete;
    Mpu9150& operator=(const Mpu9150&) = delete;
    Mpu9150(Mpu9150&&) = delete;
    Mpu9150& operator=(Mpu9150&&) = delete;

    esp_err_t init() override;
    ImuReading read() override;
    [[nodiscard]] std::string_view name() const override { return "mpu9150"; }

    // Pure register->physical conversions, exposed for host-based unit testing
    // (DESIGN.md §9 "Scaling" test) independent of I2C hardware.
    static float scaleAccel(int16_t raw);
    static float scaleGyro(int16_t raw);
    static float scaleTemp(int16_t raw);

private:
    i2c_master_dev_handle_t dev_handle_{nullptr};
    i2c_master_bus_handle_t bus_;
    uint8_t addr_;

    // Last known-good reading, held back on I2C read failure (DESIGN.md §7:
    // "I2C NACK / timeout" -> "log warning, return stale/error reading").
    ImuReading last_reading_{};

    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readRegs(uint8_t start_reg, uint8_t* buf, size_t len);
    [[nodiscard]] ImuReading snapshotLastReading() const;
};
