#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// One monitored task. `handle` may be null — DESIGN.md §9 requires such an
// entry to be skipped rather than dereferenced, which is what happens when
// app_main fails to create a task but still lists it here.
struct TaskInfo {
    std::string_view name;
    TaskHandle_t     handle;
    uint32_t         stack_total;  // Allocated stack size in bytes
};

class HealthMonitor {
public:
    HealthMonitor(httpd_handle_t server, std::vector<TaskInfo> tasks);
    ~HealthMonitor() = default;

    HealthMonitor(const HealthMonitor&) = delete;
    HealthMonitor& operator=(const HealthMonitor&) = delete;
    HealthMonitor(HealthMonitor&&) = delete;
    HealthMonitor& operator=(HealthMonitor&&) = delete;

    // Registers /health on the publisher's server. The publisher owns that
    // server's lifetime, so nothing is released here (DESIGN.md §3).
    esp_err_t init();
    void      run();  // Called by healthTask — loops every CONFIG_HEALTH_PERIOD_MS

    // Exposed for on-target testing: lets a test inspect the payload without
    // going through the HTTP stack.
    [[nodiscard]] std::string buildJson() const;

    // The health task cannot be in the list at construction — it is created
    // *with* this object as its parameter, so its handle does not exist yet.
    // app_main fills it in afterwards; until then §9's null-handle rule hides
    // the entry rather than reporting a bogus stack figure.
    bool setTaskHandle(std::string_view name, TaskHandle_t handle);

private:
    httpd_handle_t        server_;
    std::vector<TaskInfo> tasks_;
    int64_t               boot_time_{0};

    static esp_err_t healthHandler(httpd_req_t* req);
};

void healthTask(void* param);
