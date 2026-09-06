#include "HealthMonitor.hpp"

#include <cstdio>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"

namespace {
constexpr const char* kTag = "Health";

// DESIGN.md §12: a task is healthy while at least 25% of its stack is still
// free at its worst point.
constexpr float kStackHeadroomTargetPct = 25.0F;
}  // namespace

HealthMonitor::HealthMonitor(httpd_handle_t server, std::vector<TaskInfo> tasks)
    : server_(server), tasks_(std::move(tasks)) {}

esp_err_t HealthMonitor::init() {
    boot_time_ = esp_timer_get_time();

    // DESIGN.md §9: a null server is not fatal — the rest of the system runs
    // fine without the endpoint, so log and carry on rather than abort.
    if (server_ == nullptr) {
        ESP_LOGE(kTag, "no HTTP server handle, /health will not be available");
        return ESP_ERR_INVALID_ARG;
    }

    httpd_uri_t health_uri{};
    health_uri.uri = "/health";
    health_uri.method = HTTP_GET;
    health_uri.handler = healthHandler;
    health_uri.user_ctx = this;

    esp_err_t err = httpd_register_uri_handler(server_, &health_uri);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "failed to register /health: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(kTag, "/health registered");
    return ESP_OK;
}

bool HealthMonitor::setTaskHandle(std::string_view name, TaskHandle_t handle) {
    for (auto& task : tasks_) {
        if (task.name == name) {
            task.handle = handle;
            return true;
        }
    }
    ESP_LOGW(kTag, "no monitored task named '%.*s'", static_cast<int>(name.length()),
             name.data());
    return false;
}

esp_err_t HealthMonitor::healthHandler(httpd_req_t* req) {
    auto*       self = static_cast<HealthMonitor*>(req->user_ctx);
    std::string json = self->buildJson();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), static_cast<ssize_t>(json.length()));
}

std::string HealthMonitor::buildJson() const {
    const uint32_t free_heap = esp_get_free_heap_size();
    const uint32_t min_heap = esp_get_minimum_free_heap_size();
    // Not in DESIGN.md §2, but §2 states the endpoint exists to verify
    // REQ-NF-002 (<=80% heap used) — which cannot be computed from a free
    // figure alone. Reporting the total makes the requirement checkable
    // straight from the response.
    const uint32_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    const auto uptime_s =
        static_cast<uint32_t>((esp_timer_get_time() - boot_time_) / 1000000);

    wifi_ap_record_t ap_info{};
    const bool       connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    // Not a ternary: its operands promote to int, so assigning back to int8_t
    // is a narrowing conversion even though both values already fit.
    int8_t rssi = 0;
    if (connected) {
        rssi = ap_info.rssi;
    }

    // DESIGN.md §6 formats into a fixed `char buf[512]` with a running
    // `pos += snprintf(...)`. That is the same defect fixed in the publisher
    // at publisher-v1.1: snprintf returns the length it *would* have written,
    // so pos walks past the array and `sizeof(buf) - pos` wraps (size_t vs
    // int) into a huge length, writing outside buf. §9's "snprintf truncates"
    // is therefore not what happens. Built by appending instead, so the task
    // list may grow without the size becoming a correctness question.
    std::string out;
    out.reserve(128 + (tasks_.size() * 64));

    char header[192];
    snprintf(header, sizeof(header),
             R"({"uptime_s":%lu,"heap":{"free":%lu,"min_ever":%lu,"total":%lu},)"
             R"("wifi":{"rssi":%d,"connected":%s},"tasks":[)",
             static_cast<unsigned long>(uptime_s),
             static_cast<unsigned long>(free_heap),
             static_cast<unsigned long>(min_heap),
             static_cast<unsigned long>(total_heap), rssi,
             connected ? "true" : "false");
    out += header;

    bool first = true;
    for (const auto& task : tasks_) {
        if (task.handle == nullptr) {
            continue;  // DESIGN.md §9
        }
        if (!first) {
            out += ',';
        }
        first = false;

        const UBaseType_t hwm = uxTaskGetStackHighWaterMark(task.handle);
        char              entry[128];
        // %.*s because name is a string_view over a literal and is not
        // guaranteed null-terminated.
        snprintf(entry, sizeof(entry),
                 R"({"name":"%.*s","stack_hwm":%u,"stack_total":%lu})",
                 static_cast<int>(task.name.length()), task.name.data(),
                 static_cast<unsigned>(hwm),
                 static_cast<unsigned long>(task.stack_total));
        out += entry;
    }

    out += "]}";
    return out;
}

void HealthMonitor::run() {
    const TickType_t period = pdMS_TO_TICKS(CONFIG_HEALTH_PERIOD_MS);
    TickType_t       last_wake = xTaskGetTickCount();

    while (true) {
#if CONFIG_HEALTH_LOG_TO_SERIAL
        ESP_LOGI(kTag, "heap: free=%lu min_ever=%lu",
                 static_cast<unsigned long>(esp_get_free_heap_size()),
                 static_cast<unsigned long>(esp_get_minimum_free_heap_size()));

        for (const auto& task : tasks_) {
            if (task.handle == nullptr) {
                continue;
            }
            const UBaseType_t hwm = uxTaskGetStackHighWaterMark(task.handle);
            const float       pct =
                (static_cast<float>(hwm) / static_cast<float>(task.stack_total)) * 100.0F;
            // Warn rather than inform when a task breaches the REQ-NF-003
            // budget, so a shrinking stack is visible in a normal log rather
            // than needing someone to read the percentages (DESIGN.md §12).
            if (pct < kStackHeadroomTargetPct) {
                ESP_LOGW(kTag, "  %-.*s: HWM %u / %lu bytes (%.0f%% free, below %.0f%% target)",
                         static_cast<int>(task.name.length()), task.name.data(),
                         static_cast<unsigned>(hwm),
                         static_cast<unsigned long>(task.stack_total), pct,
                         kStackHeadroomTargetPct);
            } else {
                ESP_LOGI(kTag, "  %-.*s: HWM %u / %lu bytes (%.0f%% free)",
                         static_cast<int>(task.name.length()), task.name.data(),
                         static_cast<unsigned>(hwm),
                         static_cast<unsigned long>(task.stack_total), pct);
            }
        }
#endif
        vTaskDelayUntil(&last_wake, period);
    }
}

void healthTask(void* param) {
    auto* monitor = static_cast<HealthMonitor*>(param);
    monitor->run();
}
