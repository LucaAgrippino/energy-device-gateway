#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "Reading.hpp"
#include "esp_err.h"

// One entry of a slave's holding-register map.
//
// `address` is the Modbus register number, `scale` converts the raw integer to
// a physical value, and `reg_count` is 1 for a uint16 or 2 for a uint32 spanning
// two consecutive registers (big-endian, high word first) — see VISION.md §7.2.
//
// DESIGN.md §4 declares this inside ModbusRtuDevice.hpp; it lives here instead
// because the register map is transport-agnostic and DESIGN_TCP.md's
// ModbusTcpDevice takes the same map on Day 6.
struct RegisterDef {
    uint16_t         address;
    std::string_view name;
    float            scale;
    uint8_t          reg_count;
};

// DESIGN.md §3. Unlike IImu (one reading per poll), a Modbus device returns a
// variable number of readings per cycle, one per register-map entry.
class IModbusDevice {
public:
    virtual ~IModbusDevice() = default;

    virtual esp_err_t            init() = 0;
    virtual std::vector<Reading> readRegisters() = 0;
    [[nodiscard]] virtual std::string_view name() const = 0;
};
