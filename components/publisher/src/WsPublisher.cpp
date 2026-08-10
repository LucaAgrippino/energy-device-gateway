#include "WsPublisher.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"

namespace {
constexpr const char* kTag = "WsPublisher";
}  // namespace

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

WsPublisher::~WsPublisher() {
    stop();
}

esp_err_t WsPublisher::start() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = CONFIG_WS_MAX_CLIENTS;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    config.server_port = static_cast<uint16_t>(CONFIG_WS_SERVER_PORT);

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t ws_uri{};
    ws_uri.uri = "/ws";
    ws_uri.method = HTTP_GET;
    ws_uri.handler = wsHandler;
    ws_uri.user_ctx = this;
    ws_uri.is_websocket = true;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &ws_uri));

    httpd_uri_t dash_uri{};
    dash_uri.uri = "/";
    dash_uri.method = HTTP_GET;
    dash_uri.handler = dashboardHandler;
    dash_uri.user_ctx = nullptr;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server_, &dash_uri));

    return ESP_OK;
}

void WsPublisher::stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

esp_err_t WsPublisher::wsHandler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        // WebSocket handshake — just log and return OK
        ESP_LOGI(kTag, "WS client connected");
        return ESP_OK;
    }

    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;

    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "WS recv error: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(kTag, "WS frame received, len=%d, type=%d", frame.len, frame.type);
    return ESP_OK;
}

esp_err_t WsPublisher::dashboardHandler(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, reinterpret_cast<const char*>(index_html_start),
                     index_html_end - index_html_start);
    return ESP_OK;
}

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

        if (httpd_ws_get_fd_info(server_, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            esp_err_t err = httpd_ws_send_data(server_, fd, &frame);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "WS send to fd %d failed: %s", fd, esp_err_to_name(err));
            }
        }
    }
}

void WsPublisher::run() {
    while (true) {
        Snapshot snap;

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

void WsPublisher::publish(Snapshot&& snapshot) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_snapshot_ = std::move(snapshot);
        data_ready_ = true;
    }
    cv_.notify_one();
}

std::string WsPublisher::snapshotToJson(const Snapshot& snap) {
    char buf[512];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
        R"({"ts":%lld,"readings":[)", static_cast<long long>(snap.timestamp));

    for (size_t i = 0; i < snap.readings.size(); i++) {
        const auto& r = snap.readings[i];
        if (i > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += snprintf(buf + pos, sizeof(buf) - pos,
            R"({"src":"%.*s","val":%.3f,"st":%d})",
            static_cast<int>(r.source.length()), r.source.data(),
            static_cast<double>(r.value),
            static_cast<int>(r.status));
    }

    pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
    return {buf, static_cast<size_t>(pos)};
}
