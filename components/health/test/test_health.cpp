#include <string>

#include "HealthMonitor.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"

namespace {
// A real handle is needed for uxTaskGetStackHighWaterMark to return anything
// meaningful; the running test task is the most convenient one.
TaskHandle_t self() {
    return xTaskGetCurrentTaskHandle();
}
}  // namespace

TEST_CASE("HealthMonitor: init without a server is reported, not fatal", "[health]") {
    // DESIGN.md §9 — the system must keep running without the endpoint.
    HealthMonitor monitor(nullptr, {});
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, monitor.init());
}

TEST_CASE("HealthMonitor: json carries the documented top-level keys", "[health]") {
    HealthMonitor monitor(nullptr, {{"imu", self(), 4096}});
    monitor.init();  // sets boot_time_; the null-server failure is expected here

    const std::string json = monitor.buildJson();

    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"uptime_s\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"heap\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"free\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"min_ever\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"wifi\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"rssi\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"connected\":"));
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"tasks\":["));
    // Balanced and terminated — the failure mode of the buffer bug this
    // implementation deliberately avoids was a payload cut mid-token.
    TEST_ASSERT_EQUAL_CHAR('{', json.front());
    TEST_ASSERT_EQUAL_CHAR('}', json.back());
}

TEST_CASE("HealthMonitor: a null task handle is skipped", "[health]") {
    // DESIGN.md §9: a task app_main failed to create must not appear.
    HealthMonitor monitor(nullptr, {{"present", self(), 4096},
                                    {"absent", nullptr, 4096}});
    monitor.init();

    const std::string json = monitor.buildJson();

    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "\"present\""));
    TEST_ASSERT_NULL(strstr(json.c_str(), "\"absent\""));
    // Exactly one entry, so no stray separator was emitted around the skip.
    TEST_ASSERT_NULL(strstr(json.c_str(), "},{"));
}

TEST_CASE("HealthMonitor: setTaskHandle fills a late-created task in", "[health]") {
    HealthMonitor monitor(nullptr, {{"health", nullptr, 4096}});
    monitor.init();

    TEST_ASSERT_NULL(strstr(monitor.buildJson().c_str(), "\"health\""));
    TEST_ASSERT_TRUE(monitor.setTaskHandle("health", self()));
    TEST_ASSERT_NOT_NULL(strstr(monitor.buildJson().c_str(), "\"health\""));
}

TEST_CASE("HealthMonitor: setTaskHandle reports an unknown name", "[health]") {
    HealthMonitor monitor(nullptr, {{"imu", self(), 4096}});
    TEST_ASSERT_FALSE(monitor.setTaskHandle("not_a_task", self()));
}

TEST_CASE("HealthMonitor: many tasks do not truncate the payload", "[health]") {
    // The design's fixed 512-byte buffer overflowed past roughly eight tasks.
    // Sixteen comfortably exceeds that, so this fails loudly if the appending
    // build is ever swapped back for a fixed array.
    std::vector<TaskInfo> many;
    for (int i = 0; i < 16; i++) {
        many.push_back({"task_with_a_deliberately_long_name", self(), 4096});
    }
    HealthMonitor monitor(nullptr, std::move(many));
    monitor.init();

    const std::string json = monitor.buildJson();

    TEST_ASSERT_GREATER_THAN(512, json.length());
    TEST_ASSERT_EQUAL_CHAR('}', json.back());
    TEST_ASSERT_NOT_NULL(strstr(json.c_str(), "]}"));
}
