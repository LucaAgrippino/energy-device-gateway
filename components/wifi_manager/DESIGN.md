# Wi-Fi Manager Component — Design Document

**Component:** `components/wifi_manager`
**Author:** Luca Agrippino
**Date:** 2026-07-25
**Status:** Design

---

## 1. Purpose

This component manages the ESP32-S3 Wi-Fi connection lifecycle: STA mode connection
using credentials stored in NVS, automatic reconnection on disconnect, and fallback
to AP mode if STA connection fails within a configurable timeout. It exposes
connection state to other components via an event group.

**Requirements covered:** REQ-F-003, REQ-F-004, REQ-F-012, REQ-NF-005

---

## 2. State Machine

```
                    ┌──────────────┐
                    │     IDLE     │
                    └──────┬───────┘
                           │ start()
                           ▼
                    ┌──────────────┐
            ┌──────▶│ CONNECTING   │──── timeout ──┐
            │       └──────┬───────┘               │
            │              │ GOT_IP                │
            │              ▼                       ▼
            │       ┌──────────────┐       ┌──────────────┐
            │       │  CONNECTED   │       │   AP_MODE    │
            │       └──────┬───────┘       └──────┬───────┘
            │              │ DISCONNECTED         │
            │              ▼                      │ retry timer, every
            │       ┌──────────────┐              │ WIFI_AP_STA_RETRY_S,
            └───────│ RECONNECTING │              │ if NVS creds exist and
                    └──────┬───────┘              │ no AP client is attached
                           │ retries exhausted    │
                           ▼                      │
                    ┌──────────────┐              │
                    │   AP_MODE    │◀─────────────┘
                    └──────────────┘
```

**`AP_MODE` is deliberately not terminal.** An earlier revision had no arrow out
of it, which meant any outage lasting longer than `WIFI_MAX_RETRIES` allows —
roughly 12 s at the defaults — left the gateway serving a provisioning AP
forever, needing a human. A router reboot is enough to trigger it. Measured on
hardware: a 25 s AP outage stranded the device permanently, and only a reset
recovered it, which is precisely what REQ-NF-005 forbids.

| State | Description | Entry Action |
|-------|-------------|-------------|
| `IDLE` | Not started | — |
| `CONNECTING` | STA attempting to connect | `esp_wifi_connect()`, start timeout timer |
| `CONNECTED` | STA connected, IP acquired | Set `CONNECTED_BIT` in event group |
| `RECONNECTING` | STA lost connection, retrying | Increment retry counter, `esp_wifi_connect()` |
| `AP_MODE` | Fallback AP running | Start SoftAP with config SSID/password, arm the STA retry timer |

### Configurable Parameters (Kconfig)

| Parameter | Default | Kconfig Symbol |
|-----------|---------|----------------|
| STA connection timeout | 10 s | `CONFIG_WIFI_STA_TIMEOUT_S` |
| Max reconnect retries | 5 | `CONFIG_WIFI_MAX_RETRIES` |
| AP-mode STA retry period | 30 s | `CONFIG_WIFI_AP_STA_RETRY_S` |
| AP mode SSID | `"EDG-Setup"` | `CONFIG_WIFI_AP_SSID` |
| AP mode password | `"edg12345"` | `CONFIG_WIFI_AP_PASSWORD` |
| AP mode channel | 1 | `CONFIG_WIFI_AP_CHANNEL` |

---

## 3. Interface

```cpp
// WifiManager.hpp
#pragma once

#include <string_view>
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

class WifiManager {
public:
    // Event group bits — other tasks wait on these
    static constexpr int CONNECTED_BIT = BIT0;
    static constexpr int DISCONNECTED_BIT = BIT1;
    static constexpr int AP_MODE_BIT = BIT2;

    WifiManager();
    ~WifiManager();

    // Non-copyable, non-movable
    WifiManager(const WifiManager&) = delete;
    WifiManager& operator=(const WifiManager&) = delete;
    WifiManager(WifiManager&&) = delete;
    WifiManager& operator=(WifiManager&&) = delete;

    esp_err_t init();
    esp_err_t startSta(std::string_view ssid, std::string_view password);
    esp_err_t startAp();
    esp_err_t saveCredentials(std::string_view ssid, std::string_view password);
    esp_err_t loadCredentials(char* ssid, size_t ssid_len,
                              char* password, size_t pass_len);
    bool isConnected() const;
    int8_t getRssi() const;
    EventGroupHandle_t eventGroup() const { return event_group_; }

private:
    enum class State { IDLE, CONNECTING, CONNECTED, RECONNECTING, AP_MODE };

    EventGroupHandle_t event_group_{nullptr};
    esp_netif_t* sta_netif_{nullptr};
    esp_netif_t* ap_netif_{nullptr};
    State state_{State::IDLE};
    int retry_count_{0};

    static void eventHandler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
    void handleWifiEvent(int32_t event_id, void* event_data);
    void handleIpEvent(int32_t event_id, void* event_data);
    void switchToAp();
};
```

---

## 4. RAII Lifecycle

| Phase | Action | ESP-IDF API |
|-------|--------|-------------|
| Constructor | Create event group | `xEventGroupCreate()` |
| `init()` | Init NVS, TCP/IP, event loop, Wi-Fi driver, register handlers | `nvs_flash_init()`, `esp_netif_init()`, `esp_event_loop_create_default()`, `esp_wifi_init()`, `esp_event_handler_instance_register()` |
| `startSta()` | Configure STA mode, start, connect | `esp_wifi_set_mode()`, `esp_wifi_set_config()`, `esp_wifi_start()`, `esp_wifi_connect()` |
| `startAp()` | Configure AP mode, start | `esp_wifi_set_mode(WIFI_MODE_AP)`, `esp_wifi_set_config()`, `esp_wifi_start()` |
| Destructor | Unregister handlers, stop Wi-Fi, destroy netif, delete event group | `esp_event_handler_instance_unregister()`, `esp_wifi_stop()`, `esp_wifi_deinit()`, `esp_netif_destroy()`, `vEventGroupDelete()` |

### Init Sequence (in order)

```
1. nvs_flash_init()           — NVS required before Wi-Fi
2. esp_netif_init()           — TCP/IP stack
3. esp_event_loop_create_default()  — event loop for Wi-Fi events
4. esp_netif_create_default_wifi_sta()  — STA network interface
5. esp_wifi_init(&cfg)        — Wi-Fi driver with default config
6. esp_event_handler_instance_register(WIFI_EVENT, ...)
7. esp_event_handler_instance_register(IP_EVENT, ...)
8. esp_wifi_set_storage(WIFI_STORAGE_RAM)  — we manage NVS ourselves
```

Step 8 is important: by default ESP-IDF stores Wi-Fi credentials in NVS automatically. We set `WIFI_STORAGE_RAM` so we control NVS read/write explicitly through `saveCredentials()`/`loadCredentials()`.

---

## 5. Event Handler — C Callback to C++ Bridge

The ESP event loop calls a C function pointer. We bridge this to our C++ object:

```cpp
// Static C-compatible callback
void WifiManager::eventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiManager*>(arg);
    if (event_base == WIFI_EVENT) {
        self->handleWifiEvent(event_id, event_data);
    } else if (event_base == IP_EVENT) {
        self->handleIpEvent(event_id, event_data);
    }
}

// Registration passes `this` as the void* arg
esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID,
    &WifiManager::eventHandler, this,  // <-- this pointer
    &wifi_handler_instance_);
```

### Key Events

| Event Base | Event ID | Action |
|------------|----------|--------|
| `WIFI_EVENT` | `WIFI_EVENT_STA_START` | Call `esp_wifi_connect()` |
| `WIFI_EVENT` | `WIFI_EVENT_STA_DISCONNECTED` | If retries < max: reconnect. Else: switch to AP. |
| `IP_EVENT` | `IP_EVENT_STA_GOT_IP` | Set `CONNECTED_BIT`, clear `DISCONNECTED_BIT`, log IP |
| `WIFI_EVENT` | `WIFI_EVENT_AP_STACONNECTED` | Log client connected to AP |
| `WIFI_EVENT` | `WIFI_EVENT_AP_STADISCONNECTED` | Log client disconnected from AP |

---

## 6. NVS Credential Storage

```cpp
esp_err_t WifiManager::saveCredentials(std::string_view ssid,
                                        std::string_view password) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid.data());
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password.data());
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
```

### NVS Namespace and Keys

| Namespace | Key | Type | Max Length |
|-----------|-----|------|------------|
| `"wifi"` | `"ssid"` | string | 32 bytes |
| `"wifi"` | `"password"` | string | 64 bytes |

### Boot Sequence with NVS

```
1. init() → nvs_flash_init()
2. loadCredentials() → found? → startSta(ssid, password)
3. loadCredentials() → not found? → startAp() (provisioning mode)
```

---

## 7. Event Group — Inter-Task Synchronisation

Other tasks wait for Wi-Fi connectivity before starting:

```cpp
// In app_main, before starting Modbus TCP task:
xEventGroupWaitBits(
    wifi_mgr.eventGroup(),
    WifiManager::CONNECTED_BIT,
    pdFALSE,     // don't clear bit on exit
    pdTRUE,      // wait for all bits (only one here)
    portMAX_DELAY // block forever
);
// Wi-Fi is now connected — safe to start Modbus TCP
```

This is how the Wi-Fi Manager coordinates with the rest of the system without tight coupling.

---

## 8. Wi-Fi Provisioning HTTP Server

AP mode (§2) exists so a user can supply real STA credentials without a serial
connection. `startAp()` also starts a small `esp_http_server` instance —
separate from publisher's dashboard/telemetry httpd — so both can be
reachable at the same time, since sensors and the dashboard keep running
regardless of Wi-Fi state (main.cpp does not gate them on a successful STA
connection).

| Route | Method | Purpose |
|-------|--------|---------|
| `/` | GET | Serves `provisioning.html` (embedded via `EMBED_FILES`, same pattern as publisher's `dashboard/index.html`) — a form for SSID + password |
| `/save` | POST | Reads the `application/x-www-form-urlencoded` body, decodes it, calls `saveCredentials()`, responds, then `esp_restart()` |

**Port:** `CONFIG_WIFI_PROVISION_PORT` (default `8080`) — deliberately distinct
from publisher's `CONFIG_WS_SERVER_PORT` (default `80`).

**Why a second port, and not routes on the existing dashboard server:**
architecturally, Wi-Fi provisioning isn't the dashboard/telemetry publisher's
job — bolting it on there would couple two components that otherwise don't
know about each other. Two separate `esp_http_server` instances can coexist
on the same device as long as each has a **distinct `ctrl_port`** in its
`httpd_config_t` — `HTTPD_DEFAULT_CONFIG()`'s `ctrl_port` is a fixed default
shared by every instance that uses it unmodified, so a second instance must
override it (`ESP_HTTPD_DEF_CTRL_PORT + 1` here) or `httpd_start()` fails
trying to bind the same internal control socket twice.

**Non-fatal by design:** if the provisioning server fails to start (e.g. its
port is already taken during a *runtime* AP fallback that happens after the
device has been operating normally — §2's `RECONNECTING` → `AP_MODE`
transition, not just the first-boot case), this is logged as a warning, not
propagated as a fatal error. AP broadcast and the dashboard still work either
way; only the config page is unavailable in that edge case.

**URL-decoding:** `httpd_query_key_value()` only splits `key=value` pairs, it
does not decode percent-escapes or `+`-as-space. A small local `urlDecode()`
helper (anonymous namespace, `WifiManager.cpp`) handles both before the
decoded SSID/password reach `saveCredentials()`.

**Restart, not a live STA switch:** per the boot sequence (§6), the simplest
correct way to move from AP/provisioning into STA is to save credentials and
`esp_restart()` — on the next boot, `loadCredentials()` succeeds and
`startSta()` runs normally. This avoids needing to tear down AP mode and
bring up STA live from inside an HTTP handler.

---

## 9. Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| NVS init fails (corrupted) | `nvs_flash_init()` returns `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND` | Erase NVS partition, re-init |
| No credentials in NVS | `loadCredentials()` returns `ESP_ERR_NVS_NOT_FOUND` | Start AP mode directly |
| STA connection timeout | Retry count exceeds `CONFIG_WIFI_MAX_RETRIES` | Switch to AP mode, set `AP_MODE_BIT`, arm the retry timer |
| AP running but the network returns | `CONFIG_WIFI_AP_STA_RETRY_S` elapses with stored credentials present and no AP client attached | Stop the provisioning server, re-attempt STA. A failure re-enters AP mode and re-arms the timer, so the loop continues indefinitely |
| STA disconnected (runtime) | `WIFI_EVENT_STA_DISCONNECTED` | Auto-reconnect, clear `CONNECTED_BIT`, set `DISCONNECTED_BIT` |
| DHCP timeout | No `IP_EVENT_STA_GOT_IP` within timeout | Treated as connection failure, retry |

### NVS Erase-and-Recover Pattern

```cpp
esp_err_t err = nvs_flash_init();
if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
    err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS corrupted, erasing...");
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
}
ESP_ERROR_CHECK(err);
```

---

## 10. File Structure

```
components/wifi_manager/
├── CMakeLists.txt
├── DESIGN.md              ← this file
├── Kconfig
├── include/
│   └── WifiManager.hpp
├── src/
│   └── WifiManager.cpp
└── test/
    └── test_wifi_manager.cpp
```

### CMakeLists.txt

```cmake
idf_component_register(
    SRCS "src/WifiManager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif nvs_flash
    PRIV_REQUIRES common
)
```

### Kconfig

```kconfig
menu "Wi-Fi Manager Configuration"

    config WIFI_STA_TIMEOUT_S
        int "STA connection timeout (seconds)"
        default 10

    config WIFI_MAX_RETRIES
        int "Max STA reconnect retries before AP fallback"
        default 5

    config WIFI_AP_SSID
        string "AP mode SSID"
        default "EDG-Setup"

    config WIFI_AP_PASSWORD
        string "AP mode password"
        default "edg12345"

    config WIFI_AP_CHANNEL
        int "AP mode channel"
        default 1
        range 1 13

endmenu
```

---

## 11. Integration Tests

| Test | Description | Type |
|------|-------------|------|
| STA connect | Provide valid credentials, verify `CONNECTED_BIT` set, `getRssi()` returns valid value | On-target integration |
| STA reconnect | Connect, disable AP briefly, verify auto-reconnect | On-target integration |
| AP fallback | Provide invalid credentials, verify AP mode starts after timeout | On-target integration |
| NVS persist | Save credentials, reboot, verify `loadCredentials()` returns them | On-target integration |
| NVS empty | Erase NVS, boot, verify AP mode starts | On-target integration |
| RAII cleanup | Construct, init, destroy, verify no resource leaks | On-target integration |

---

## 12. Stack and Heap Budget

| Metric | Target | Measurement Method |
|--------|--------|--------------------|
| Stack HWM | ≥ 25% of 4096 = 1024 bytes free | `uxTaskGetStackHighWaterMark()` |
| Heap impact | Wi-Fi driver allocates ~40–60 KB internally | `esp_get_free_heap_size()` before/after init |

Note: The Wi-Fi driver is the single largest heap consumer in the system. This is expected and accounted for in the RAM budget.
