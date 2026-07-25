#pragma once

#include <cstdint>
#include <string_view>

struct Reading {
    std::string_view source;     // e.g. "imu.accel_x", "modbus_rtu.voltage"
    float            value;
    int64_t          timestamp;  // esp_timer_get_time() in µs
    enum class Status { OK, TIMEOUT, ERROR } status;

    Reading(Reading&&) = default;
    Reading& operator=(Reading&&) = default;
    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;
};
