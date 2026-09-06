# Common Types Component — Design Document

**Component:** `components/common`
**Author:** Luca Agrippino
**Date:** 2026-07-26
**Status:** Design

---

## 1. Purpose

This component defines the shared data types used across all other components.
It is header-only — no `.cpp` files, no compilation, no RAII resources. It exists
to ensure consistent data structures between producers (sensors, Modbus) and
consumers (aggregator, publisher).

---

## 2. Reading.hpp

```cpp
#pragma once

#include <cstdint>
#include <string_view>

struct Reading {
    std::string_view source;     // e.g. "imu.accel_x", "modbus_rtu.voltage"
    float            value;
    int64_t          timestamp;  // esp_timer_get_time() in µs
    enum class Status : uint8_t { OK = 0, TIMEOUT = 1, ERROR = 2 } status;

    Reading() = default;
    Reading(Reading&&) = default;
    Reading& operator=(Reading&&) = default;
    Reading(const Reading&) = delete;
    Reading& operator=(const Reading&) = delete;
};
```

### Design Notes

- **Move-only:** enforces one-way data flow at the type level. `xQueueOverwrite`
  bypasses this via `memcpy` — safe because all fields are trivially copyable.
- **`string_view source`:** points to compile-time string literals (`"imu.accel_x"`).
  Zero allocation. The literals live in flash for the entire program.
- **`Status` as `enum class : uint8_t`:** scoped, type-safe, 1 byte. The integer
  values (0, 1, 2) are used directly in JSON serialisation.
- **`int64_t timestamp`:** microseconds since boot from `esp_timer_get_time()`.
  64-bit avoids overflow for ~292,000 years.

### Memory Layout

```
| Field       | Offset | Size    |
|-------------|--------|---------|
| source.ptr  | 0      | 4 bytes |
| source.len  | 4      | 4 bytes |
| value       | 8      | 4 bytes |
| timestamp   | 12     | 8 bytes |  (may have 4 bytes padding before)
| status      | 20     | 1 byte  |
| (padding)   | 21     | 3 bytes |
| Total       |        | ~24 bytes |
```

Exact layout depends on compiler alignment. Use `sizeof(Reading)` to verify.

---

## 3. Snapshot.hpp

```cpp
#pragma once

#include <cstdint>
#include <vector>
#include "Reading.hpp"

struct Snapshot {
    int64_t              timestamp;   // µs since boot
    std::vector<Reading> readings;    // One per data source

    Snapshot() = default;
    Snapshot(Snapshot&&) = default;
    Snapshot& operator=(Snapshot&&) = default;
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
};
```

### Design Notes

- **Move-only:** `Snapshot` contains a `vector<Reading>`. Since `Reading` is
  move-only, `Snapshot` must also be move-only. The aggregator builds a fresh
  snapshot each cycle and moves it to the publisher.
- **`std::vector<Reading>`:** dynamically sized because the number of readings
  depends on how many sensors and registers are configured. Typically ~10 entries.

---

## 4. DeviceConfig.hpp

```cpp
#pragma once

#include <cstdint>
#include <string_view>
#include "driver/gpio.h"
#include "driver/uart.h"

struct I2cConfig {
    gpio_num_t sda;
    gpio_num_t scl;
    uint32_t   freq_hz;
};

struct ModbusRtuConfig {
    uart_port_t port;
    uint8_t     slave_addr;
    gpio_num_t  tx_pin;
    gpio_num_t  rx_pin;
    gpio_num_t  de_re_pin;
    uint32_t    baud_rate;
};

struct ModbusTcpConfig {
    std::string_view ip_addr;
    uint16_t         port;
    uint8_t          unit_id;
};
```

These config structs centralize hardware configuration. They're populated from
Kconfig values in `app_main` and passed to the respective constructors.

---

## 5. File Structure

```
components/common/
├── include/
│   ├── Reading.hpp
│   ├── Snapshot.hpp
│   └── DeviceConfig.hpp
└── CMakeLists.txt
```

### CMakeLists.txt

```cmake
idf_component_register(
    INCLUDE_DIRS "include"
)
```

No `SRCS` — this is a header-only component. Other components use
`PRIV_REQUIRES common` or `REQUIRES common` to access these headers.
