#pragma once

#include <cstdint>
#include <string_view>

struct Reading {
    std::string_view source;             // e.g. "imu.accel_x", "modbus_rtu.voltage"
    float            value{0.0f};
    int64_t          timestamp{0};       // esp_timer_get_time() in µs
    enum class Status { OK, TIMEOUT, ERROR } status{Status::TIMEOUT};

    // aggregator_DESIGN.md §5 default-constructs a Reading before filling it
    // in from a mailbox peek (or the empty-mailbox fallback), so this needs
    // to be usable with no arguments — hence the default member initializers
    // above and this defaulted constructor.
    Reading() = default;

    // Reading has user-declared move/copy special members, so it isn't an
    // aggregate under this project's C++ standard (gnu++2b) — brace-init
    // needs this constructor to actually work.
    Reading(std::string_view source, float value, int64_t timestamp, Status status)
        : source(source), value(value), timestamp(timestamp), status(status) {}

    Reading(Reading&&) = default;
    Reading& operator=(Reading&&) = default;
    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;
};
