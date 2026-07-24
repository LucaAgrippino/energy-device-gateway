# IMU Component — Design Document

**Component:** `components/imu`
**Author:** Luca Agrippino
**Date:** 2026-07-24
**Status:** Design

---

## 1. Purpose

This component provides an abstraction layer for reading accelerometer and gyroscope
data from an IMU over I2C. The initial target is the Drotek MPU9150, which is
register-compatible with the MPU6050 for accel/gyro reads.

**Requirements covered:** REQ-F-001, REQ-F-002

---

## 2. Hardware

| Item | Value |
|------|-------|
| Module | Drotek MPU9150 (MPU6050 + AK8975 magnetometer) |
| Interface | I2C, up to 400 kHz |
| I2C address | `0x69` (Drotek board, AD0 pulled high) |
| WHO_AM_I register | `0x75`, expected value `0x68` |
| Supply voltage | 3.3V (Drotek board has onboard regulator, accepts 3.3–5V) |
| Pull-ups | Onboard on Drotek board — no external resistors needed |
| ESP32-S3 SDA | GPIO 1 |
| ESP32-S3 SCL | GPIO 2 |

### Key Registers (MPU6050-register-compatible)

| Register | Address | Description |
|----------|---------|-------------|
| PWR_MGMT_1 | `0x6B` | Power management. Write `0x00` to wake from sleep. |
| WHO_AM_I | `0x75` | Device ID. Returns `0x68`. |
| ACCEL_XOUT_H | `0x3B` | Start of 6-byte accel burst read (X, Y, Z, 2 bytes each). |
| GYRO_XOUT_H | `0x43` | Start of 6-byte gyro burst read (X, Y, Z, 2 bytes each). |
| ACCEL_CONFIG | `0x1C` | Full-scale range: ±2g (0x00), ±4g (0x08), ±8g (0x10), ±16g (0x18). |
| GYRO_CONFIG | `0x1B` | Full-scale range: ±250°/s (0x00), ±500°/s (0x08), ±1000°/s (0x10), ±2000°/s (0x18). |
| SMPRT_DIV | `0x19` | Sample rate divider. Rate = 1 kHz / (1 + SMPRT_DIV). |

### Burst Read Strategy

Read 14 bytes starting at `0x3B` in a single I2C transaction:

```
0x3B–0x40: ACCEL_X_H, ACCEL_X_L, ACCEL_Y_H, ACCEL_Y_L, ACCEL_Z_H, ACCEL_Z_L
0x41–0x42: TEMP_H, TEMP_L
0x43–0x48: GYRO_X_H, GYRO_X_L, GYRO_Y_H, GYRO_Y_L, GYRO_Z_H, GYRO_Z_L
```

This is more efficient than 6 separate register reads and guarantees all axes
are sampled from the same measurement cycle.

---

## 3. Interface

```cpp
// IImu.hpp
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
    virtual std::string_view name() const = 0;
};
```

---

## 4. Mpu9150 Driver

```cpp
// Mpu9150.hpp
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
    std::string_view name() const override { return "mpu9150"; }

private:
    i2c_master_dev_handle_t dev_handle_{nullptr};
    i2c_master_bus_handle_t bus_;
    uint8_t addr_;

    esp_err_t writeReg(uint8_t reg, uint8_t value);
    esp_err_t readRegs(uint8_t start_reg, uint8_t* buf, size_t len);
};
```

### RAII Lifecycle

| Phase | Action | ESP-IDF API |
|-------|--------|-------------|
| Constructor | Add device to I2C bus | `i2c_master_bus_add_device()` |
| `init()` | Wake from sleep, verify WHO_AM_I, configure ranges | Register writes |
| `read()` | 14-byte burst read, scale raw values | Register reads |
| Destructor | Remove device from I2C bus | `i2c_master_bus_rm_device()` |

### Scaling

| Measurement | Raw → Physical | Default Range |
|-------------|----------------|---------------|
| Accelerometer | raw / 16384.0 × 9.80665 (m/s²) | ±2g (sensitivity 16384 LSB/g) |
| Gyroscope | raw / 131.0 (°/s) | ±250°/s (sensitivity 131 LSB/°/s) |
| Temperature | raw / 340.0 + 36.53 (°C) | — |

---

## 5. I2C Bus Ownership

The I2C master bus is created in `app_main` and passed to the `Mpu9150` constructor.
The IMU does **not** own the bus — it only owns its device handle on that bus.

```cpp
// In app_main:
i2c_master_bus_config_t bus_cfg = {
    .i2c_port = I2C_NUM_0,
    .sda_io_num = GPIO_NUM_1,
    .scl_io_num = GPIO_NUM_2,
    .clk_source = I2C_CLK_SRC_DEFAULT,
    .glitch_ignore_cnt = 7,
};
i2c_master_bus_handle_t bus;
ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

auto imu = std::make_unique<Mpu9150>(bus, 0x69);
ESP_ERROR_CHECK(imu->init());
```

---

## 6. ImuTask

```cpp
void imuTask(void* param) {
    auto* ctx = static_cast<ImuTaskContext*>(param);
    const TickType_t period = pdMS_TO_TICKS(100);  // 10 Hz
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        ImuReading reading = ctx->imu->read();

        // Convert to Reading structs for the aggregator mailbox
        Reading accel_x{"imu.accel_x", reading.accel_x,
                        reading.timestamp, Reading::Status::OK};
        // ... repeat for accel_y, accel_z, gyro_x, gyro_y, gyro_z

        xQueueOverwrite(ctx->mailbox, &accel_x);
        // ... one mailbox per reading channel, or a single ImuReading mailbox

        vTaskDelayUntil(&last_wake, period);
    }
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 5 | Highest sensor priority (per VISION.md §6.1) |
| Stack | 4096 bytes | To be verified with HWM measurement |
| Period | 100 ms (10 Hz) | Configurable via Kconfig |

### Mailbox Strategy

Two options — to be decided during implementation:

**(A) Single mailbox with `ImuReading`:** One `xQueueOverwrite` call per cycle.
Aggregator receives the full IMU snapshot atomically. Simpler.

**(B) Per-channel mailboxes:** One mailbox per axis (accel_x, accel_y, ...).
More granular timeout detection. Matches Modbus pattern (per-register readings).

Option A is preferred for Day 1 — simpler, and `ImuReading` is small enough
for a single mailbox (56 bytes).

---

## 7. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| WHO_AM_I mismatch | `init()` reads `0x75`, compares to `0x68` | Return `ESP_ERR_NOT_FOUND`, log error |
| I2C NACK / timeout | ESP-IDF returns `ESP_ERR_TIMEOUT` or `ESP_FAIL` | Log warning, return stale/error reading |
| Device not responding | Repeated read failures | `ImuTask` marks readings as `Status::ERROR` |
| I2C bus error | SDA stuck low | Needs power cycle — logged as critical |

---

## 8. File Structure

```
components/imu/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── include/
│   ├── IImu.hpp
│   └── Mpu9150.hpp
├── src/
│   └── Mpu9150.cpp
└── test/
    └── test_mpu9150.cpp
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/Mpu9150.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES common
)
```

---

## 9. Unit Tests

| Test | Description | Type |
|------|-------------|------|
| WHO_AM_I probe | Read `0x75`, verify returns `0x68` | On-target integration |
| Init wake-up | Write `0x00` to PWR_MGMT_1, read back, verify not in sleep | On-target integration |
| Burst read | Read 14 bytes from `0x3B`, verify non-zero accel_z (gravity) | On-target integration |
| Scaling | Feed known raw values, verify physical output (e.g. raw 16384 → 9.81 m/s²) | Host-based unit |
| RAII cleanup | Construct, destroy, verify no I2C bus leak | On-target integration |

---

## 10. Stack and Heap Budget

To be measured after implementation. Targets from VISION.md:

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | `uxTaskGetStackHighWaterMark()` |
| Heap impact | Minimal (stack-allocated readings) | `esp_get_free_heap_size()` before/after |
