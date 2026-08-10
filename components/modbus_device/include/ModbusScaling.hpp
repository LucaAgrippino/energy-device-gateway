#pragma once

#include <cstdint>

// Raw holding-register values to physical units.
//
// Shared by both transports: RTU and TCP frame their requests differently and
// the payload sits at a different offset in each reply, but once the bytes are
// located the conversion is identical (DESIGN_TCP.md §5). Kept as free
// functions so neither device has to depend on the other for arithmetic.
namespace modbus {

// One register holding a uint16.
inline float scaleValue(uint16_t raw, float scale) {
    return static_cast<float>(raw) * scale;
}

// Two consecutive registers holding a uint32, high word first (big-endian),
// per the register map in VISION.md §7.2.
inline float scaleValue32(uint16_t high, uint16_t low, float scale) {
    const uint32_t raw = (static_cast<uint32_t>(high) << 16) | low;
    return static_cast<float>(raw) * scale;
}

}  // namespace modbus
