#pragma once

#include <vector>

#include "IModbusDevice.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

// Task period: 0.5 Hz (DESIGN_TCP.md §6 "Task Parameters").
constexpr TickType_t kModbusTcpTaskPeriod = pdMS_TO_TICKS(CONFIG_MODBUS_TCP_POLL_MS);

// One mailbox per register-map entry, in the same order as the map — the same
// fan-out ModbusRtuTask uses, and for the same reason: a std::vector cannot
// travel through xQueueOverwrite without leaving the aggregator holding a
// pointer to a freed buffer.
//
// Unlike RTU, this task needs the Wi-Fi event group: there is no point opening
// a socket before the station has an IP, and polls are skipped while the link
// is down rather than burning a connect timeout every cycle
// (DESIGN_TCP.md §6).
struct ModbusTcpTaskContext {
    IModbusDevice*             device{nullptr};
    EventGroupHandle_t         wifi_event_group{nullptr};
    int                        connected_bit{0};
    std::vector<QueueHandle_t> mailboxes;
};

void modbusTcpTask(void* param);
