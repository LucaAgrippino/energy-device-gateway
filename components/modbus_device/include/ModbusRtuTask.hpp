#pragma once

#include <vector>

#include "IModbusDevice.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Task period: 1 Hz (DESIGN.md §10 "Task Parameters").
constexpr TickType_t kModbusRtuTaskPeriod = pdMS_TO_TICKS(1000);

// One mailbox per register-map entry, in the same order as the map the device
// was constructed with.
//
// DESIGN.md §10 sketches a single mailbox carrying the whole
// std::vector<Reading> through xQueueOverwrite. That can't work: the queue
// memcpy's the vector object (pointer, size, capacity) and the vector then
// frees its buffer when it leaves scope, so the aggregator would dereference
// freed memory. Fanning out one POD Reading per mailbox is the pattern already
// established for the IMU in commit ab6c8e6, and it is what the aggregator's
// MailboxEntry model expects.
struct ModbusRtuTaskContext {
    IModbusDevice*             device{nullptr};
    std::vector<QueueHandle_t> mailboxes;
};

void modbusRtuTask(void* param);
