#include "ModbusTcpTask.hpp"

#include <algorithm>
#include <vector>

#include "Reading.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace {
constexpr const char* kTag = "ModbusTcpTask";

// Publish a full set of TIMEOUT readings so the dashboard shows the source as
// unavailable while Wi-Fi is down, rather than freezing on the last good value
// until the aggregator's own stale timeout eventually trips.
void publishUnavailable(const ModbusTcpTaskContext& ctx) {
    const int64_t now = esp_timer_get_time();
    for (auto* mailbox : ctx.mailboxes) {
        Reading reading{"modbus_tcp", 0.0F, now, Reading::Status::TIMEOUT};
        xQueueOverwrite(mailbox, &reading);
    }
}
}  // namespace

void modbusTcpTask(void* param) {
    auto* ctx = static_cast<ModbusTcpTaskContext*>(param);

    // Block until the station actually has a connection. init() opens a socket,
    // which cannot succeed before then (DESIGN_TCP.md §6).
    ESP_LOGI(kTag, "waiting for Wi-Fi before connecting to the Modbus TCP slave");
    xEventGroupWaitBits(ctx->wifi_event_group, ctx->connected_bit, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    // A failure here is not fatal: readRegisters() reconnects lazily, so a slave
    // that is not up yet simply reports ERROR until it appears.
    esp_err_t err = ctx->device->init();
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "initial connect failed: %s — will retry each poll",
                 esp_err_to_name(err));
    }

    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        const EventBits_t bits = xEventGroupGetBits(ctx->wifi_event_group);
        if ((bits & ctx->connected_bit) == 0) {
            ESP_LOGW(kTag, "Wi-Fi down, skipping poll");
            publishUnavailable(*ctx);
            vTaskDelayUntil(&last_wake, kModbusTcpTaskPeriod);
            continue;
        }

        std::vector<Reading> readings = ctx->device->readRegisters();

        if (readings.size() != ctx->mailboxes.size()) {
            // The mailbox list and the register map disagree, which is a wiring
            // bug in app_main rather than a network problem.
            ESP_LOGW(kTag, "%u readings for %u mailboxes",
                     static_cast<unsigned>(readings.size()),
                     static_cast<unsigned>(ctx->mailboxes.size()));
        }

        const size_t count = std::min(readings.size(), ctx->mailboxes.size());
        for (size_t i = 0; i < count; i++) {
            xQueueOverwrite(ctx->mailboxes[i], &readings[i]);
        }

        vTaskDelayUntil(&last_wake, kModbusTcpTaskPeriod);
    }
}
