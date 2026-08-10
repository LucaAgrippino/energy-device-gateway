#include "WifiManager.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {
constexpr const char* kTag = "WifiManager";

// Decodes application/x-www-form-urlencoded text in place: '+' -> space,
// "%XX" -> the byte XX. Used on the SSID/password fields from the
// provisioning form's POST body (DESIGN.md §8) since httpd_query_key_value()
// only splits key/value pairs, it doesn't decode percent-escapes.
void urlDecode(const char* src, char* dst, size_t dst_size) {
    size_t di = 0;
    for (size_t si = 0; src[si] != '\0' && di + 1 < dst_size; si++) {
        if (src[si] == '+') {
            dst[di++] = ' ';
        } else if (src[si] == '%' && isxdigit(static_cast<unsigned char>(src[si + 1])) &&
                   isxdigit(static_cast<unsigned char>(src[si + 2]))) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = static_cast<char>(strtol(hex, nullptr, 16));
            si += 2;
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}
}  // namespace

extern const uint8_t provisioning_html_start[] asm("_binary_provisioning_html_start");
extern const uint8_t provisioning_html_end[] asm("_binary_provisioning_html_end");

WifiManager::WifiManager() : event_group_(xEventGroupCreate()) {}

WifiManager::~WifiManager() {
    stopProvisioningServer();
    if (sta_timeout_timer_ != nullptr) {
        esp_timer_stop(sta_timeout_timer_);
        esp_timer_delete(sta_timeout_timer_);
    }
    if (wifi_handler_instance_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance_);
    }
    if (ip_handler_instance_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler_instance_);
    }

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "esp_wifi_stop failed: %s", esp_err_to_name(err));
    }
    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(kTag, "esp_wifi_deinit failed: %s", esp_err_to_name(err));
    }

    if (sta_netif_ != nullptr) esp_netif_destroy(sta_netif_);
    if (ap_netif_ != nullptr) esp_netif_destroy(ap_netif_);
    if (event_group_ != nullptr) vEventGroupDelete(event_group_);
}

esp_err_t WifiManager::init() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK) return err;

    sta_netif_ = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &WifiManager::eventHandler, this,
                                               &wifi_handler_instance_);
    if (err != ESP_OK) return err;

    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &WifiManager::eventHandler, this,
                                               &ip_handler_instance_);
    if (err != ESP_OK) return err;

    return esp_wifi_set_storage(WIFI_STORAGE_RAM);
}

esp_err_t WifiManager::startSta(std::string_view ssid, std::string_view password) {
    wifi_config_t wifi_config{};
    size_t ssid_len = std::min(ssid.size(), sizeof(wifi_config.sta.ssid) - 1);
    memcpy(wifi_config.sta.ssid, ssid.data(), ssid_len);

    size_t pass_len = std::min(password.size(), sizeof(wifi_config.sta.password) - 1);
    memcpy(wifi_config.sta.password, password.data(), pass_len);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    state_ = State::CONNECTING;
    retry_count_ = 0;

    esp_timer_create_args_t timer_args{};
    timer_args.callback = &WifiManager::staTimeoutCallback;
    timer_args.arg = this;
    timer_args.name = "wifi_sta_timeout";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &sta_timeout_timer_));
    ESP_ERROR_CHECK(esp_timer_start_once(
        sta_timeout_timer_, static_cast<uint64_t>(CONFIG_WIFI_STA_TIMEOUT_S) * 1000000ULL));

    // esp_wifi_connect() itself is triggered from handleWifiEvent() on
    // WIFI_EVENT_STA_START (DESIGN.md §5), once esp_wifi_start() brings the
    // STA interface up.
    return ESP_OK;
}

esp_err_t WifiManager::startAp() {
    if (ap_netif_ == nullptr) {
        // Not created in init()'s documented sequence (DESIGN.md §4) since
        // AP mode is only needed as a fallback — created lazily here instead.
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }

    std::string_view ssid = CONFIG_WIFI_AP_SSID;
    std::string_view password = CONFIG_WIFI_AP_PASSWORD;

    wifi_config_t wifi_config{};
    size_t ssid_len = std::min(ssid.size(), sizeof(wifi_config.ap.ssid));
    memcpy(wifi_config.ap.ssid, ssid.data(), ssid_len);
    wifi_config.ap.ssid_len = static_cast<uint8_t>(ssid_len);

    size_t pass_len = std::min(password.size(), sizeof(wifi_config.ap.password) - 1);
    memcpy(wifi_config.ap.password, password.data(), pass_len);

    wifi_config.ap.channel = CONFIG_WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (err != ESP_OK) return err;

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    state_ = State::AP_MODE;
    xEventGroupSetBits(event_group_, AP_MODE_BIT);
    ESP_LOGI(kTag, "AP mode started: SSID=%.*s", static_cast<int>(ssid.size()), ssid.data());

    err = startProvisioningServer();
    if (err != ESP_OK) {
        // Non-fatal (DESIGN.md §8): AP broadcast still works even without a
        // config page, e.g. if port CONFIG_WIFI_PROVISION_PORT is already
        // taken during a runtime fallback that happens after the dashboard
        // httpd is already running.
        ESP_LOGW(kTag, "provisioning server failed to start: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t WifiManager::saveCredentials(std::string_view ssid, std::string_view password) {
    char ssid_buf[33];
    char pass_buf[65];

    size_t ssid_len = std::min(ssid.size(), sizeof(ssid_buf) - 1);
    memcpy(ssid_buf, ssid.data(), ssid_len);
    ssid_buf[ssid_len] = '\0';

    size_t pass_len = std::min(password.size(), sizeof(pass_buf) - 1);
    memcpy(pass_buf, password.data(), pass_len);
    pass_buf[pass_len] = '\0';

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid_buf);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", pass_buf);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t WifiManager::loadCredentials(char* ssid, size_t ssid_len,
                                       char* password, size_t pass_len) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &pass_len);
    }
    nvs_close(handle);
    return err;
}

bool WifiManager::isConnected() const {
    return state_ == State::CONNECTED;
}

int8_t WifiManager::getRssi() const {
    wifi_ap_record_t ap_info{};
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

void WifiManager::eventHandler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    auto* self = static_cast<WifiManager*>(arg);
    if (event_base == WIFI_EVENT) {
        self->handleWifiEvent(event_id, event_data);
    } else if (event_base == IP_EVENT) {
        self->handleIpEvent(event_id, event_data);
    }
}

void WifiManager::handleWifiEvent(int32_t event_id, void* /*event_data*/) {
    switch (event_id) {
        case WIFI_EVENT_STA_START:
            state_ = State::CONNECTING;
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            xEventGroupClearBits(event_group_, CONNECTED_BIT);
            xEventGroupSetBits(event_group_, DISCONNECTED_BIT);
            if (retry_count_ < CONFIG_WIFI_MAX_RETRIES) {
                retry_count_++;
                state_ = State::RECONNECTING;
                ESP_LOGW(kTag, "STA disconnected, retry %d/%d", retry_count_, CONFIG_WIFI_MAX_RETRIES);
                esp_wifi_connect();
            } else {
                ESP_LOGW(kTag, "STA retries exhausted, falling back to AP mode");
                switchToAp();
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(kTag, "client connected to AP");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(kTag, "client disconnected from AP");
            break;
        default:
            break;
    }
}

void WifiManager::handleIpEvent(int32_t event_id, void* event_data) {
    if (event_id != IP_EVENT_STA_GOT_IP) return;

    if (sta_timeout_timer_ != nullptr) {
        esp_timer_stop(sta_timeout_timer_);
        esp_timer_delete(sta_timeout_timer_);
        sta_timeout_timer_ = nullptr;
    }

    auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    state_ = State::CONNECTED;
    retry_count_ = 0;
    xEventGroupClearBits(event_group_, DISCONNECTED_BIT);
    xEventGroupSetBits(event_group_, CONNECTED_BIT);
    ESP_LOGI(kTag, "connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

void WifiManager::switchToAp() {
    if (sta_timeout_timer_ != nullptr) {
        esp_timer_stop(sta_timeout_timer_);
        esp_timer_delete(sta_timeout_timer_);
        sta_timeout_timer_ = nullptr;
    }
    // STA is already running at this point; it must be stopped before the
    // driver can be reconfigured into AP mode.
    esp_wifi_stop();

    esp_err_t err = startAp();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to start AP fallback: %s", esp_err_to_name(err));
    }
}

void WifiManager::staTimeoutCallback(void* arg) {
    auto* self = static_cast<WifiManager*>(arg);
    if (self->state_ == State::CONNECTED) return;  // race guard: GOT_IP won the race
    ESP_LOGW(kTag, "STA connection timed out after %d s, falling back to AP mode",
             CONFIG_WIFI_STA_TIMEOUT_S);
    self->switchToAp();
}

esp_err_t WifiManager::startProvisioningServer() {
    if (provisioning_server_ != nullptr) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = static_cast<uint16_t>(CONFIG_WIFI_PROVISION_PORT);
    // HTTPD_DEFAULT_CONFIG()'s ctrl_port is a fixed default shared by every
    // httpd instance regardless of server_port; since WsPublisher's dashboard
    // httpd may already be running (DESIGN.md §8), this second instance needs
    // its own ctrl_port or httpd_start() fails trying to bind the same one.
    config.ctrl_port = static_cast<uint16_t>(ESP_HTTPD_DEF_CTRL_PORT + 1);

    esp_err_t err = httpd_start(&provisioning_server_, &config);
    if (err != ESP_OK) {
        provisioning_server_ = nullptr;
        return err;
    }

    httpd_uri_t root_uri{};
    root_uri.uri = "/";
    root_uri.method = HTTP_GET;
    root_uri.handler = provisionRootHandler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(provisioning_server_, &root_uri));

    httpd_uri_t save_uri{};
    save_uri.uri = "/save";
    save_uri.method = HTTP_POST;
    save_uri.handler = provisionSaveHandler;
    save_uri.user_ctx = this;
    ESP_ERROR_CHECK(httpd_register_uri_handler(provisioning_server_, &save_uri));

    ESP_LOGI(kTag, "provisioning server listening on port %d", CONFIG_WIFI_PROVISION_PORT);
    return ESP_OK;
}

void WifiManager::stopProvisioningServer() {
    if (provisioning_server_ != nullptr) {
        httpd_stop(provisioning_server_);
        provisioning_server_ = nullptr;
    }
}

esp_err_t WifiManager::provisionRootHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, reinterpret_cast<const char*>(provisioning_html_start),
                     provisioning_html_end - provisioning_html_start);
    return ESP_OK;
}

esp_err_t WifiManager::provisionSaveHandler(httpd_req_t* req) {
    auto* self = static_cast<WifiManager*>(req->user_ctx);

    char body[160];
    const auto total_len = static_cast<int>(req->content_len);
    if (total_len <= 0 || total_len >= static_cast<int>(sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
        return ESP_FAIL;
    }
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, body + received, total_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[total_len] = '\0';

    char ssid_raw[33] = {};
    char pass_raw[65] = {};
    if (httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    // Password is optional (open networks); leave pass_raw empty if absent.
    httpd_query_key_value(body, "password", pass_raw, sizeof(pass_raw));

    char ssid[33];
    char password[65];
    urlDecode(ssid_raw, ssid, sizeof(ssid));
    urlDecode(pass_raw, password, sizeof(password));

    esp_err_t err = self->saveCredentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "saveCredentials failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to save credentials");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    static const char kResp[] = "Credentials saved. Restarting...";
    httpd_resp_send(req, kResp, sizeof(kResp) - 1);

    ESP_LOGI(kTag, "credentials saved via provisioning form, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP response flush before reboot
    esp_restart();
    return ESP_OK;  // unreachable
}
