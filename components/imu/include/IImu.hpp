#pragma once

#include <cstdint>
#include <string_view>
#include "esp_err.h"

struct ImuReading {
    float accel_x, accel_y, accel_z;  // m/s²
    float gyro_x, gyro_y, gyro_z;    // °/s
    float temperature;                // °C
    int64_t timestamp;                // µs, from esp_timer_get_time()

    ImuReading() = default;
    ImuReading(ImuReading&&) = default;
    ImuReading& operator=(ImuReading&&) = default;
    ImuReading(const ImuReading&) = delete;
    ImuReading& operator=(const ImuReading&) = delete;
};

class IImu {
public:
    virtual ~IImu() = default;
    virtual esp_err_t init() = 0;
    virtual ImuReading read() = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};
