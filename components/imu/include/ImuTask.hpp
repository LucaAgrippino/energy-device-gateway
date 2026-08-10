#pragma once

#include "IImu.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Task period: 10 Hz (DESIGN.md §6 "Task Parameters").
constexpr TickType_t kImuTaskPeriod = pdMS_TO_TICKS(100);

// Each mailbox holds a single common::Reading (aggregator_DESIGN.md's
// one-Reading-per-mailbox model — xQueueCreate(1, sizeof(Reading)), updated
// via xQueueOverwrite). ImuReading's 7 fields don't fit as one mailbox, so
// imuTask fans each read() out across 7 mailboxes, one per axis/temp.
struct ImuTaskContext {
    IImu* imu;
    QueueHandle_t accel_x_mailbox;
    QueueHandle_t accel_y_mailbox;
    QueueHandle_t accel_z_mailbox;
    QueueHandle_t gyro_x_mailbox;
    QueueHandle_t gyro_y_mailbox;
    QueueHandle_t gyro_z_mailbox;
    QueueHandle_t temp_mailbox;
};

void imuTask(void* param);
