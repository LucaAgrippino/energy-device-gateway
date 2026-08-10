#include "ImuTask.hpp"

#include "Reading.hpp"
#include "freertos/task.h"

namespace {
void writeReading(QueueHandle_t mailbox, std::string_view source, float value, int64_t timestamp) {
    Reading reading{source, value, timestamp, Reading::Status::OK};
    xQueueOverwrite(mailbox, &reading);
}
}  // namespace

void imuTask(void* param) {
    auto* ctx = static_cast<ImuTaskContext*>(param);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        ImuReading r = ctx->imu->read();

        writeReading(ctx->accel_x_mailbox, "imu.accel_x", r.accel_x, r.timestamp);
        writeReading(ctx->accel_y_mailbox, "imu.accel_y", r.accel_y, r.timestamp);
        writeReading(ctx->accel_z_mailbox, "imu.accel_z", r.accel_z, r.timestamp);
        writeReading(ctx->gyro_x_mailbox, "imu.gyro_x", r.gyro_x, r.timestamp);
        writeReading(ctx->gyro_y_mailbox, "imu.gyro_y", r.gyro_y, r.timestamp);
        writeReading(ctx->gyro_z_mailbox, "imu.gyro_z", r.gyro_z, r.timestamp);
        writeReading(ctx->temp_mailbox, "imu.temp", r.temperature, r.timestamp);

        vTaskDelayUntil(&last_wake, kImuTaskPeriod);
    }
}
