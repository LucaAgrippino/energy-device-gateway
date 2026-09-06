# Health Component — Design Document

**Component:** `components/health`
**Author:** Luca Agrippino
**Date:** 2026-07-26
**Status:** Design

---

## 1. Purpose

This component provides a `/health` HTTP endpoint that reports system diagnostics:
free heap, minimum heap ever, Wi-Fi RSSI, uptime, and task stack high-water marks.
It runs as a low-priority periodic task and registers its handler on the HTTP server
owned by the publisher.

**Requirements covered:** REQ-F-011, REQ-NF-002, REQ-NF-003

---

## 2. /health Endpoint Response

```json
{
  "uptime_s": 3600,
  "heap": {
    "free": 185320,
    "min_ever": 162048
  },
  "wifi": {
    "rssi": -42,
    "connected": true
  },
  "tasks": [
    {"name": "imu",         "stack_hwm": 1580, "stack_total": 4096},
    {"name": "modbus_rtu",  "stack_hwm": 1320, "stack_total": 4096},
    {"name": "modbus_tcp",  "stack_hwm": 1480, "stack_total": 4096},
    {"name": "aggregator",  "stack_hwm": 1240, "stack_total": 4096},
    {"name": "publisher",   "stack_hwm": 2100, "stack_total": 6144},
    {"name": "health",      "stack_hwm": 1680, "stack_total": 4096}
  ]
}
```

This endpoint is designed for operational monitoring and debugging — check it
during development to verify REQ-NF-002 (≤80% heap) and REQ-NF-003 (≥25% stack
headroom).

---

## 3. Interface

```cpp
// HealthMonitor.hpp
#pragma once

#include <vector>
#include <string_view>
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct TaskInfo {
    std::string_view name;
    TaskHandle_t     handle;
    uint32_t         stack_total;   // Allocated stack size in bytes
};

class HealthMonitor {
public:
    HealthMonitor(httpd_handle_t server, std::vector<TaskInfo> tasks);
    ~HealthMonitor() = default;

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;
    HealthMonitor(HealthMonitor&&) = delete;
    HealthMonitor& operator=(HealthMonitor&&) = delete;

    esp_err_t init();
    void run();   // Called by HealthTask — loops every 5 seconds

private:
    httpd_handle_t server_;
    std::vector<TaskInfo> tasks_;
    int64_t boot_time_;

    static esp_err_t healthHandler(httpd_req_t* req);
    std::string buildJson();
};
```

### Design Decisions

- **No RAII for HTTP handler:** the handler is registered on the publisher's server.
  The publisher owns the server lifetime. HealthMonitor just registers a URI.
- **TaskHandle_t stored at construction:** `app_main` creates all tasks and passes
  their handles to HealthMonitor. This avoids runtime task discovery.
- **Periodic logging + HTTP endpoint:** the `run()` loop logs health data every 5s
  to serial (useful during development). The HTTP endpoint serves the same data on
  demand.

---

## 4. Gathering Metrics

### Heap

```cpp
uint32_t free_heap = esp_get_free_heap_size();
uint32_t min_heap = esp_get_minimum_free_heap_size();
```

`esp_get_minimum_free_heap_size()` returns the lowest the heap has ever been since
boot — the true high-water mark for memory pressure. If this drops below 20% of
total heap, you have a memory problem.

### Wi-Fi RSSI

```cpp
wifi_ap_record_t ap_info;
esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
int8_t rssi = (err == ESP_OK) ? ap_info.rssi : 0;
```

RSSI (Received Signal Strength Indicator) is measured in dBm:

| RSSI | Signal Quality |
|------|----------------|
| -30 to -50 | Excellent |
| -50 to -60 | Good |
| -60 to -70 | Fair |
| -70 to -80 | Weak |
| < -80 | Very weak / disconnecting |

### Uptime

```cpp
int64_t now = esp_timer_get_time();   // µs since boot
uint32_t uptime_s = static_cast<uint32_t>((now - boot_time_) / 1'000'000);
```

### Task Stack High-Water Mark

```cpp
for (const auto& task : tasks_) {
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(task.handle);
    // hwm = minimum free stack EVER (in bytes on ESP32)
    float headroom = static_cast<float>(hwm) / task.stack_total * 100.0f;
    ESP_LOGI(TAG, "Task %-15s: HWM %5u / %5u bytes (%.0f%% free)",
             task.name.data(), hwm, task.stack_total, headroom);
}
```

Target: every task maintains ≥25% headroom. If a task's HWM drops below 25%,
increase its stack allocation.

---

## 5. HTTP Handler

```cpp
esp_err_t HealthMonitor::healthHandler(httpd_req_t* req) {
    auto* self = static_cast<HealthMonitor*>(req->user_ctx);
    std::string json = self->buildJson();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json.c_str(), json.length());
    return ESP_OK;
}
```

Registration (in `init()`):

```cpp
esp_err_t HealthMonitor::init() {
    boot_time_ = esp_timer_get_time();

    httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = healthHandler,
        .user_ctx = this,
    };
    return httpd_register_uri_handler(server_, &health_uri);
}
```

---

## 6. JSON Building

```cpp
std::string HealthMonitor::buildJson() {
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();
    int64_t now = esp_timer_get_time();
    uint32_t uptime_s = static_cast<uint32_t>((now - boot_time_) / 1'000'000);

    wifi_ap_record_t ap_info;
    bool connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    int8_t rssi = connected ? ap_info.rssi : 0;

    char buf[512];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"uptime_s\":%lu,\"heap\":{\"free\":%lu,\"min_ever\":%lu},"
        "\"wifi\":{\"rssi\":%d,\"connected\":%s},\"tasks\":[",
        uptime_s, free_heap, min_heap,
        rssi, connected ? "true" : "false");

    for (size_t i = 0; i < tasks_.size(); i++) {
        UBaseType_t hwm = uxTaskGetStackHighWaterMark(tasks_[i].handle);
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"name\":\"%.*s\",\"stack_hwm\":%u,\"stack_total\":%lu}",
            static_cast<int>(tasks_[i].name.length()), tasks_[i].name.data(),
            hwm, tasks_[i].stack_total);
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return std::string(buf, pos);
}
```

---

## 7. HealthTask

```cpp
void HealthMonitor::run() {
    const TickType_t period = pdMS_TO_TICKS(5000);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        // Log to serial for development
        ESP_LOGI(TAG, "Heap: free=%lu min=%lu",
                 esp_get_free_heap_size(), esp_get_minimum_free_heap_size());

        for (const auto& task : tasks_) {
            UBaseType_t hwm = uxTaskGetStackHighWaterMark(task.handle);
            float pct = static_cast<float>(hwm) / task.stack_total * 100.0f;
            ESP_LOGI(TAG, "  %-15.*s: HWM %5u / %5lu (%.0f%%)",
                     static_cast<int>(task.name.length()), task.name.data(),
                     hwm, task.stack_total, pct);
        }

        vTaskDelayUntil(&last_wake, period);
    }
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 1 | Lowest — monitoring shouldn't preempt real work |
| Stack | 4096 bytes | JSON building + logging |
| Period | 5000 ms | Low frequency is sufficient for monitoring |
| Core | tskNO_AFFINITY | No core preference — runs wherever there's time |

---

## 8. Kconfig

```kconfig
menu "Health Monitor Configuration"

    config HEALTH_PERIOD_MS
        int "Health check period (ms)"
        default 5000
        range 1000 30000

    config HEALTH_LOG_TO_SERIAL
        bool "Log health data to serial"
        default y

endmenu
```

---

## 9. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| Server handle is null | Check at init | Log error, don't register handler (system still runs) |
| Task handle is null | Check in `buildJson` | Skip that task in the output |
| Wi-Fi not connected | `esp_wifi_sta_get_ap_info` fails | Report `rssi: 0`, `connected: false` |
| JSON buffer overflow | `snprintf` truncates | 512 bytes handles ~8 tasks; increase if more tasks added |

---

## 10. File Structure

```
components/health/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── Kconfig
├── include/
│   └── HealthMonitor.hpp
├── src/
│   └── HealthMonitor.cpp
└── test/
    └── test_health.cpp
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/HealthMonitor.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_wifi freertos
    PRIV_REQUIRES common
)
```

---

## 11. Integration Tests

| Test | Description | Type |
|------|-------------|------|
| HTTP response | GET `/health`, verify valid JSON with expected fields | On-target (`curl`) |
| Heap values | Verify `free` > 0 and `min_ever` ≤ `free` | On-target |
| Stack HWM | Verify all tasks have ≥25% headroom | On-target |
| RSSI | Connect to Wi-Fi, verify RSSI is in valid range (-100 to 0) | On-target |
| Uptime | Wait 10s, verify `uptime_s` ≥ 10 | On-target |

---

## 12. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | Self-reported via `/health` |
| Heap impact | JSON buffer 512 bytes on stack, `std::string` temporary | Minimal |
