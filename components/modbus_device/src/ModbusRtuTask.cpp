#include "ModbusRtuTask.hpp"

#include <algorithm>
#include <vector>

#include "Reading.hpp"
#include "esp_log.h"
#include "freertos/task.h"

namespace {
constexpr const char* kTag = "ModbusRtuTask";
}  // namespace

void modbusRtuTask(void* param) {
    auto* ctx = static_cast<ModbusRtuTaskContext*>(param);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        std::vector<Reading> readings = ctx->device->readRegisters();

        if (readings.size() != ctx->mailboxes.size()) {
            // Would mean the mailbox list and the register map disagree, i.e. a
            // wiring bug in app_main rather than a bus problem.
            ESP_LOGW(kTag, "%u readings for %u mailboxes",
                     static_cast<unsigned>(readings.size()),
                     static_cast<unsigned>(ctx->mailboxes.size()));
        }

        const size_t count = std::min(readings.size(), ctx->mailboxes.size());
        for (size_t i = 0; i < count; i++) {
            xQueueOverwrite(ctx->mailboxes[i], &readings[i]);
        }

        vTaskDelayUntil(&last_wake, kModbusRtuTaskPeriod);
    }
}
