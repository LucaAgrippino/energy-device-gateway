# Aggregator Component — Design Document

**Component:** `components/aggregator`
**Author:** Luca Agrippino
**Date:** 2026-07-25
**Status:** Design

---

## 1. Purpose

This component collects the latest readings from all data sources (IMU, Modbus RTU,
Modbus TCP) via FreeRTOS mailboxes, assembles them into a `Snapshot`, detects stale
readings, and signals the publisher with the completed snapshot. It is the central
data hub of the system.

**Requirements covered:** REQ-F-008, REQ-F-009, REQ-NF-001

---

## 2. Architecture

```
 ImuTask ──mailbox──┐
                    │
 ModbusRtuTask ──mailbox──┤     AggregatorTask        PublisherTask
                    │         │                          │
 ModbusTcpTask ──mailbox──┘    peek all mailboxes       │
                               check timestamps         │
                               build Snapshot            │
                               publish(snapshot) ────────→ wait() → broadcast
                               sleep 500 ms
```

### Data Flow

1. Each sensor task calls `xQueueOverwrite` to write its latest reading to a
   dedicated mailbox (FreeRTOS queue of length 1).
2. Every 500 ms, the aggregator peeks all mailboxes with `xQueuePeek` (non-destructive
   read — the data stays in the mailbox for the next cycle).
3. For each reading, the aggregator checks if the timestamp is stale (older than
   a configurable timeout).
4. All readings are assembled into a `Snapshot`.
5. The snapshot is published to `WsPublisher` via `std::condition_variable`.

### Why xQueuePeek (Not xQueueReceive)?

`xQueueReceive` removes the item from the queue. If the sensor task hasn't produced
a new reading since the last aggregator cycle (e.g., Modbus RTU runs at 1 Hz,
aggregator at 2 Hz), the receive would fail. `xQueuePeek` reads without removing,
so the aggregator always gets the latest value even if it hasn't changed.

---

## 3. Interface

```cpp
// Aggregator.hpp
#pragma once

#include <vector>
#include <cstdint>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "Snapshot.hpp"
#include "Reading.hpp"
#include "WsPublisher.hpp"

struct MailboxEntry {
    QueueHandle_t   queue;         // The FreeRTOS mailbox
    std::string_view name;         // Source name for logging
    int64_t         timeout_us;    // Stale threshold in µs
};

class Aggregator {
public:
    Aggregator(std::vector<MailboxEntry> mailboxes, WsPublisher& publisher);
    ~Aggregator() = default;

    Aggregator(const Aggregator&) = delete;
    Aggregator& operator=(const Aggregator&) = delete;
    Aggregator(Aggregator&&) = delete;
    Aggregator& operator=(Aggregator&&) = delete;

    void run();   // Called by AggregatorTask — loops forever

private:
    std::vector<MailboxEntry> mailboxes_;
    WsPublisher& publisher_;

    Snapshot collectSnapshot();
    Reading::Status checkStale(int64_t reading_ts, int64_t now, int64_t timeout) const;
};
```

---

## 4. Mailbox Registration

Mailboxes are created by `app_main` and passed to the aggregator at construction.
The aggregator does NOT own the mailboxes — they are shared between sensor tasks
(writers) and the aggregator (reader).

```cpp
// In app_main:

// IMU mailbox (single ImuReading)
QueueHandle_t imu_mailbox = xQueueCreate(1, sizeof(ImuReading));

// Modbus RTU mailbox (per-register Reading)
QueueHandle_t rtu_mailbox = xQueueCreate(1, sizeof(Reading));

// Modbus TCP mailbox
QueueHandle_t tcp_mailbox = xQueueCreate(1, sizeof(Reading));

std::vector<MailboxEntry> mailboxes = {
    {imu_mailbox, "imu", 500'000},          // 500 ms timeout
    {rtu_mailbox, "modbus_rtu", 3'000'000}, // 3 s timeout (1 Hz poll)
    {tcp_mailbox, "modbus_tcp", 5'000'000}, // 5 s timeout (0.5 Hz poll)
};

Aggregator aggregator(std::move(mailboxes), publisher);
```

### Timeout Values

| Source | Poll Period | Stale Timeout | Rationale |
|--------|-----------|---------------|-----------|
| IMU | 100 ms | 500 ms (500,000 µs) | 5 missed cycles |
| Modbus RTU | 1000 ms | 3000 ms (3,000,000 µs) | 3 missed cycles |
| Modbus TCP | 2000 ms | 5000 ms (5,000,000 µs) | 2.5 missed cycles |

---

## 5. Core Logic — collectSnapshot()

```cpp
Snapshot Aggregator::collectSnapshot() {
    Snapshot snap;
    snap.timestamp = esp_timer_get_time();  // µs since boot

    for (auto& entry : mailboxes_) {
        Reading reading;

        // Non-blocking peek — returns pdTRUE if mailbox has data
        if (xQueuePeek(entry.queue, &reading, 0) == pdTRUE) {
            // Staleness only downgrades; the producer's own status stands
            // otherwise, so a device-reported ERROR survives to the dashboard.
            if (isStale(reading.timestamp, snap.timestamp, entry.timeout_us)) {
                reading.status = Reading::Status::TIMEOUT;
            }
        } else {
            // Mailbox empty — no data ever received from this source
            reading.source = entry.name;
            reading.value = 0.0f;
            reading.timestamp = 0;
            reading.status = Reading::Status::TIMEOUT;
        }

        snap.readings.push_back(std::move(reading));
    }

    return snap;
}

bool Aggregator::isStale(int64_t reading_ts, int64_t now, int64_t timeout) {
    return now - reading_ts > timeout;
}
```

### The Aggregator Loop

```cpp
void Aggregator::run() {
    const TickType_t period = pdMS_TO_TICKS(500);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        Snapshot snap = collectSnapshot();
        publisher_.publish(snap);

        vTaskDelayUntil(&last_wake, period);
    }
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 3 | Below sensors, above publisher |
| Stack | 4096 bytes | Verify with HWM — vector operations use stack |
| Period | 500 ms | Configurable via Kconfig |
| Core | 1 | Keep off core 0 (Wi-Fi) |

---

## 6. Kconfig

```kconfig
menu "Aggregator Configuration"

    config AGGREGATOR_PERIOD_MS
        int "Aggregation period (ms)"
        default 500
        range 100 5000

    config IMU_STALE_TIMEOUT_MS
        int "IMU stale timeout (ms)"
        default 500

    config MODBUS_RTU_STALE_TIMEOUT_MS
        int "Modbus RTU stale timeout (ms)"
        default 3000

    config MODBUS_TCP_STALE_TIMEOUT_MS
        int "Modbus TCP stale timeout (ms)"
        default 5000

endmenu
```

---

## 7. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| Mailbox empty | `xQueuePeek` returns `pdFALSE` | Mark reading as TIMEOUT, value = 0 |
| Stale reading | Timestamp older than timeout threshold | Set status to TIMEOUT, keep last known value |
| Producer reported a failure | Reading arrives with `Status::ERROR` (e.g. `ModbusRtuDevice::allFailed`) | **Keep it.** Staleness may only downgrade a reading's status, never upgrade it — a fresh timestamp is not evidence of health, because a failing device republishes on every poll and so never appears stale |

### Status Precedence

A reading's final status is decided in this order:

1. **Mailbox empty** → `TIMEOUT` (nothing has ever arrived from this source).
2. **Stale** → `TIMEOUT` (not having heard from a source recently is a stronger
   statement than whatever its last message happened to say).
3. **Otherwise** → whatever the producer set: `OK`, or `ERROR`/`TIMEOUT` if the
   device diagnosed its own failure.

The aggregator never *raises* a status. It can only report the source's verdict
or a worse one of its own.
| Publisher not responding | `publish()` blocks if publisher mutex is held | Bounded by publisher task wakeup; aggregator doesn't block on publish |
| Heap exhaustion | `snap.readings.push_back` throws `std::bad_alloc` | Unlikely — snapshot size is fixed and small (~10 readings) |

### Non-Blocking Guarantee (REQ-NF-001)

The aggregator never blocks on any single source:
- `xQueuePeek` with timeout `0` returns immediately if the mailbox is empty
- A stale or missing source produces a TIMEOUT reading — the snapshot is still built
- The publisher receives the full snapshot regardless of which sources are healthy

---

## 8. Snapshot Type (Recap)

```cpp
// Snapshot.hpp (in components/common/include/)
struct Snapshot {
    int64_t                timestamp;   // µs since boot
    std::vector<Reading>   readings;    // One per data source
};
```

The Snapshot must be copyable (the publisher receives a copy via `publish()`). Since
`Reading` is move-only, the aggregator builds a fresh Snapshot each cycle and moves
each Reading into the vector. The publisher then receives the Snapshot by value
(the aggregator doesn't keep old snapshots).

**Design note:** This means `Snapshot` itself needs to be movable. `std::vector<Reading>`
is movable even though `Reading` is move-only, because `vector::push_back` can accept
rvalue references.

---

## 9. Host-Based Testing

The aggregator has no hardware dependencies — it reads from FreeRTOS queues and
writes to the publisher. This makes it testable on the host (Linux) without an ESP32.

```
components/aggregator/
├── host_test/
│   ├── CMakeLists.txt
│   └── test_aggregator.cpp
```

### Host Test CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(host_test_aggregator)
```

### Building and Running

```bash
cd components/aggregator/host_test
idf.py set-target linux
idf.py build
./build/host_test_aggregator.elf
```

### Test Cases

| Test | Description | Type |
|------|-------------|------|
| Fresh readings | Write readings to mailboxes, collect, verify all Status::OK | Host-based unit |
| Stale detection | Write reading with old timestamp, verify Status::TIMEOUT | Host-based unit |
| Empty mailbox | Don't write to a mailbox, verify Status::TIMEOUT with value 0 | Host-based unit |
| Multiple sources | 3 mailboxes, verify snapshot contains 3 readings in order | Host-based unit |
| Timestamp | Verify snapshot timestamp is close to `esp_timer_get_time()` | Host-based unit |

### On-Target Tests

| Test | Description | Type |
|------|-------------|------|
| IMU → Aggregator | Wire IMU, verify accel_z ≈ 9.81 in snapshot | On-target integration |
| Full pipeline | IMU + Modbus + Aggregator + Publisher → browser receives JSON | On-target system (Day 7) |

---

## 10. File Structure

```
components/aggregator/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── Kconfig
├── include/
│   └── Aggregator.hpp
├── src/
│   └── Aggregator.cpp
├── test/
│   └── test_aggregator.cpp
└── host_test/
    ├── CMakeLists.txt
    └── main/
        ├── CMakeLists.txt
        └── test_aggregator.cpp
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/Aggregator.cpp"
    INCLUDE_DIRS "include"
    REQUIRES freertos
    PRIV_REQUIRES common publisher
)
```

---

## 11. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | `uxTaskGetStackHighWaterMark()` |
| Heap impact | Snapshot vector (~10 Readings × 32 bytes = ~320 bytes per cycle) | `esp_get_free_heap_size()` |
