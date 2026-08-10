#pragma once

#include <string_view>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] int8_t getRssi() const;
    [[nodiscard]] EventGroupHandle_t eventGroup() const { return event_group_; }

private:
    enum class State { IDLE, CONNECTING, CONNECTED, RECONNECTING, AP_MODE };

    EventGroupHandle_t event_group_{nullptr};
    esp_netif_t* sta_netif_{nullptr};
    esp_netif_t* ap_netif_{nullptr};
    State state_{State::IDLE};
    int retry_count_{0};

    // Not shown in DESIGN.md §3's interface sketch, but required to
    // unregister handlers in the destructor (§4 RAII table) and to implement
    // the CONNECTING state's "start timeout timer" entry action (§2).
    esp_event_handler_instance_t wifi_handler_instance_{nullptr};
    esp_event_handler_instance_t ip_handler_instance_{nullptr};
    esp_timer_handle_t sta_timeout_timer_{nullptr};

    // Provisioning HTTP server (AP mode only) — separate port from
    // publisher's dashboard (DESIGN.md §8) so both can run at once, since
    // sensors/telemetry keep running regardless of Wi-Fi state.
    httpd_handle_t provisioning_server_{nullptr};

    static void eventHandler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);
    void handleWifiEvent(int32_t event_id, void* event_data);
    void handleIpEvent(int32_t event_id, void* event_data);
    void switchToAp();
    static void staTimeoutCallback(void* arg);

    esp_err_t startProvisioningServer();
    void stopProvisioningServer();
    static esp_err_t provisionRootHandler(httpd_req_t* req);
    static esp_err_t provisionSaveHandler(httpd_req_t* req);
};
