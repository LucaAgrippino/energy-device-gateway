# WebSocket Publisher Component — Design Document

**Component:** `components/publisher`
**Author:** Luca Agrippino
**Date:** 2026-07-25
**Status:** Design

---

## 1. Purpose

This component runs the HTTP server and WebSocket endpoint that streams JSON
telemetry to connected browser clients. It also serves the static HTML dashboard.
The publisher receives snapshots from the aggregator via `std::condition_variable`
and broadcasts them as JSON frames to all connected WebSocket clients.

**Requirements covered:** REQ-F-005, REQ-F-010, REQ-NF-004

---

## 2. Architecture

```
AggregatorTask                 PublisherTask                    Clients
    │                              │                              │
    │  lock mutex                  │                              │
    │  write snapshot_             │                              │
    │  notify_one()                │                              │
    │                              │  wait(lock) ← wakes up      │
    │                              │  copy snapshot under lock    │
    │                              │  serialise to JSON           │
    │                              │  for each connected client:  │
    │                              │    httpd_ws_send_data()  ──────→ Browser
    │                              │                              │
```

### Key Design Decisions

- **C++ condition_variable for aggregator→publisher** — demonstrates portable C++
  concurrency as specified in VISION.md §6.2.
- **httpd_ws_send_data (synchronous)** — simpler than async for our throughput
  (one frame every 500 ms to a handful of clients).
- **Client tracking via httpd_get_client_list()** — no manual FD tracking needed.
  ESP-IDF's httpd provides the list of connected sockets; we filter for WebSocket clients.

---

## 3. Interface

```cpp
// WsPublisher.hpp
#pragma once

#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include "esp_http_server.h"
#include "Snapshot.hpp"

class WsPublisher {
public:
    WsPublisher();
    ~WsPublisher();

    WsPublisher(const WsPublisher&) = delete;
    WsPublisher& operator=(const WsPublisher&) = delete;
    WsPublisher(WsPublisher&&) = delete;
    WsPublisher& operator=(WsPublisher&&) = delete;

    esp_err_t start();
    void stop();

    // Called by AggregatorTask (producer side)
    void publish(const Snapshot& snapshot);

    // Called by PublisherTask (consumer side)
    void run();

    httpd_handle_t serverHandle() const { return server_; }

private:
    httpd_handle_t server_{nullptr};
    Snapshot latest_snapshot_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool data_ready_{false};

    static esp_err_t wsHandler(httpd_req_t* req);
    static esp_err_t dashboardHandler(httpd_req_t* req);
    void broadcastJson(const std::string& json);
    std::string snapshotToJson(const Snapshot& snap);
};
```

---

## 4. HTTP Server Setup

### Endpoints

| Path | Method | Type | Handler | Purpose |
|------|--------|------|---------|---------|
| `/ws` | GET | WebSocket | `wsHandler` | Telemetry stream |
| `/` | GET | HTTP | `dashboardHandler` | Serve HTML dashboard |
| `/health` | GET | HTTP | (Health component) | Heap, RSSI, stack HWM |

### Server Configuration

```cpp
esp_err_t WsPublisher::start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;        // Max simultaneous WebSocket clients
    config.lru_purge_enable = true;     // Close oldest client if max reached
    config.stack_size = 8192;           // httpd task stack

    ESP_ERROR_CHECK(httpd_start(&server_, &config));

    // Register WebSocket endpoint
    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = wsHandler,
        .user_ctx = this,
        .is_websocket = true,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &ws_uri));

    // Register dashboard endpoint
    httpd_uri_t dash_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = dashboardHandler,
        .user_ctx = nullptr,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &dash_uri));

    return ESP_OK;
}
```

### Kconfig

```kconfig
menu "Publisher Configuration"

    config WS_MAX_CLIENTS
        int "Max WebSocket clients"
        default 4
        range 1 8

    config WS_SERVER_PORT
        int "HTTP server port"
        default 80

endmenu
```

### sdkconfig.defaults

```ini
CONFIG_HTTPD_WS_SUPPORT=y
```

WebSocket support is disabled by default in ESP-IDF. Must be explicitly enabled.

---

## 5. WebSocket Handler

The handler is called by the httpd task for each incoming WebSocket frame.
For our use case, we mostly ignore incoming frames (the dashboard doesn't send
commands) — the handler exists to complete the WebSocket handshake and keep the
connection alive.

```cpp
esp_err_t WsPublisher::wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // WebSocket handshake — just log and return OK
        ESP_LOGI(TAG, "WS client connected");
        return ESP_OK;
    }

    // Receive frame (ping/pong or client message)
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS recv error: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "WS frame received, len=%d, type=%d", frame.len, frame.type);
    return ESP_OK;
}
```

---

## 6. Broadcasting to All Clients

The publisher must send data to ALL connected WebSocket clients, not just the one
that triggered a request. This requires using `httpd_get_client_list()` and
`httpd_ws_send_data()`:

```cpp
void WsPublisher::broadcastJson(const std::string& json) {
    if (!server_) return;

    size_t max_clients = CONFIG_WS_MAX_CLIENTS;
    int client_fds[CONFIG_WS_MAX_CLIENTS];
    size_t num_clients = max_clients;

    if (httpd_get_client_list(server_, &num_clients, client_fds) != ESP_OK) {
        return;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json.c_str())),
        .len = json.length(),
    };

    for (size_t i = 0; i < num_clients; i++) {
        int fd = client_fds[i];

        // Check if this client is a WebSocket client (not a regular HTTP client)
        if (httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            esp_err_t err = httpd_ws_send_data(server_, fd, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "WS send to fd %d failed: %s", fd, esp_err_to_name(err));
            }
        }
    }
}
```

---

## 7. Publisher Task — Consumer Side

```cpp
void WsPublisher::run() {
    while (true) {
        Snapshot snap;

        // Wait for new data from aggregator
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return data_ready_; });
            snap = std::move(latest_snapshot_);
            data_ready_ = false;
        }

        // Serialise and broadcast (outside the lock)
        std::string json = snapshotToJson(snap);
        broadcastJson(json);
    }
}

// Called by AggregatorTask
void WsPublisher::publish(const Snapshot& snapshot) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_snapshot_ = snapshot;
        data_ready_ = true;
    }
    cv_.notify_one();
}
```

### Task Parameters

| Parameter | Value | Notes |
|-----------|-------|-------|
| Priority | 2 | Lower than sensors, higher than health |
| Stack | 6144 bytes | JSON serialisation uses stack; verify HWM |
| Core | 1 | Keep off core 0 (Wi-Fi) |

---

## 8. JSON Serialisation

No external JSON library. Simple `snprintf`-based formatting — sufficient for our
fixed schema and avoids heap-heavy libraries like cJSON.

```cpp
std::string WsPublisher::snapshotToJson(const Snapshot& snap) {
    char buf[512];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "{\"ts\":%lld,\"readings\":[", snap.timestamp);

    for (size_t i = 0; i < snap.readings.size(); i++) {
        const auto& r = snap.readings[i];
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            "{\"src\":\"%.*s\",\"val\":%.3f,\"st\":%d}",
            static_cast<int>(r.source.length()), r.source.data(),
            r.value,
            static_cast<int>(r.status));
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return std::string(buf, pos);
}
```

### Example JSON Output

```json
{
  "ts": 1234567890,
  "readings": [
    {"src": "imu.accel_x", "val": 0.123, "st": 0},
    {"src": "imu.accel_y", "val": -0.045, "st": 0},
    {"src": "imu.accel_z", "val": 9.810, "st": 0},
    {"src": "modbus_rtu.voltage", "val": 48.5, "st": 0},
    {"src": "modbus_tcp.power", "val": 1200.0, "st": 1}
  ]
}
```

Status values: `0` = OK, `1` = TIMEOUT, `2` = ERROR.

---

## 9. Static Dashboard

The HTML dashboard is embedded in the firmware binary via CMakeLists.txt:

```cmake
idf_component_register(
    SRCS "src/WsPublisher.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server
    PRIV_REQUIRES common
    EMBED_FILES "../../dashboard/index.html"
)
```

`EMBED_FILES` converts the HTML file into a linkable binary blob accessible via:

```cpp
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

esp_err_t WsPublisher::dashboardHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, reinterpret_cast<const char*>(index_html_start),
                    index_html_end - index_html_start);
    return ESP_OK;
}
```

---

## 10. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| httpd_start fails | Returns non-ESP_OK | Log error, publisher disabled (system still runs sensors) |
| Client disconnect | `httpd_ws_send_data` returns error | Log warning, skip client (httpd removes it automatically) |
| JSON buffer overflow | `snprintf` truncates | Increase buffer size. 512 bytes handles ~15 readings. |
| No clients connected | `num_clients == 0` or none are WS | Skip broadcast, no error |
| Wi-Fi disconnects | Server keeps running on TCP stack | Clients will reconnect when Wi-Fi returns |

---

## 11. File Structure

```
components/publisher/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── Kconfig
├── include/
│   └── WsPublisher.hpp
├── src/
│   └── WsPublisher.cpp
└── test/
    └── test_ws_publisher.cpp

dashboard/
└── index.html             ← Embedded into firmware binary
```

---

## 12. Integration Tests

| Test | Description | Type |
|------|-------------|------|
| WS handshake | Connect to `ws://<ip>/ws`, verify handshake succeeds | On-target (Python `websocket-client`) |
| JSON frame | Connect, wait for frame, verify valid JSON with expected fields | On-target |
| Multi-client | Connect 4 clients, verify all receive the same frame | On-target |
| Dashboard | HTTP GET `/`, verify HTML response | On-target (`curl`) |
| Disconnect recovery | Connect, disconnect, reconnect, verify frames resume | On-target |
| Latency | Measure time from `publish()` call to frame received by client | On-target (REQ-NF-004: ≤ 500 ms) |

---

## 13. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM (publisher task) | ≥ 25% of 6144 = 1536 bytes free | `uxTaskGetStackHighWaterMark()` |
| Stack HWM (httpd task) | ≥ 25% of 8192 | `uxTaskGetStackHighWaterMark()` |
| Heap impact | JSON buffer ~512 bytes stack, `std::string` temporary on heap | `esp_get_free_heap_size()` |
