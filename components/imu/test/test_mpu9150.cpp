#include <cmath>

#include "Mpu9150.hpp"
#include "driver/i2c_master.h"
#include "unity.h"

namespace {
constexpr gpio_num_t kSdaPin = GPIO_NUM_1;
constexpr gpio_num_t kSclPin = GPIO_NUM_2;
constexpr uint8_t kImuAddr = 0x69;

i2c_master_bus_handle_t openTestBus() {
    i2c_master_bus_config_t bus_cfg{};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = kSdaPin;
    bus_cfg.scl_io_num = kSclPin;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;

    i2c_master_bus_handle_t bus;
    TEST_ESP_OK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}
}  // namespace

// The tests below marked [hw] require a Drotek MPU9150 wired to I2C0
// (SDA=GPIO1, SCL=GPIO2, DESIGN.md §2). Run via the ESP-IDF unit-test-app,
// on-target.
//
// Each [hw] case releases the I2C bus *before* asserting anything about the
// device. Unity's TEST_ESP_OK aborts the case with longjmp, which does not
// unwind C++ destructors — so asserting first left the bus acquired, and every
// later case then failed with "I2C bus id(0) has already been acquired"
// instead of its own result. One unresponsive sensor was reported as three
// unrelated failures, two of which never actually ran.

TEST_CASE("MPU9150 init wakes device and passes WHO_AM_I probe", "[imu][hw]") {
    i2c_master_bus_handle_t bus = openTestBus();
    esp_err_t init_err = ESP_FAIL;
    {
        Mpu9150 imu(bus, kImuAddr);
        init_err = imu.init();
    }  // destructor removes the device here, before the bus is deleted
    TEST_ESP_OK(i2c_del_master_bus(bus));

    TEST_ESP_OK(init_err);
}

TEST_CASE("MPU9150 burst read measures 1g total while stationary", "[imu][hw]") {
    // Previously asserted accel_z ~ 9.81, which only holds with the board lying
    // flat on the bench and failed whenever it was propped or on its side —
    // a property of how the board was placed, not of the driver. The magnitude
    // of the acceleration vector is 1g in *any* orientation while stationary,
    // so it tests the same thing (device woken, range configured, all three
    // axes scaled correctly) without depending on the bench.
    i2c_master_bus_handle_t bus = openTestBus();
    esp_err_t init_err = ESP_FAIL;
    ImuReading reading{};  // value-initialised: read() is skipped if init fails
    {
        Mpu9150 imu(bus, kImuAddr);
        init_err = imu.init();
        if (init_err == ESP_OK) {
            reading = imu.read();
        }
    }  // destructor removes the device here, before the bus is deleted
    TEST_ESP_OK(i2c_del_master_bus(bus));

    TEST_ESP_OK(init_err);

    const float magnitude = std::sqrt((reading.accel_x * reading.accel_x) +
                                      (reading.accel_y * reading.accel_y) +
                                      (reading.accel_z * reading.accel_z));
    // +/-1.0 absorbs sensor noise and a small residual bias; a wrong scale
    // factor or a dead axis moves this far further than that.
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 9.80665f, magnitude);
}

TEST_CASE("MPU9150 RAII: construct/destroy releases the I2C device cleanly", "[imu][hw]") {
    i2c_master_bus_handle_t bus = openTestBus();
    esp_err_t init_err = ESP_FAIL;
    {
        Mpu9150 imu(bus, kImuAddr);
        init_err = imu.init();
    }  // destructor removes the device here

    // The point of this case: the bus deletes cleanly, which it only can if the
    // destructor detached the device. That holds whether or not the sensor
    // answered, so it is asserted before the init result.
    TEST_ESP_OK(i2c_del_master_bus(bus));

    TEST_ESP_OK(init_err);
}

TEST_CASE("MPU9150 scaling: known raw values map to expected physical units", "[imu]") {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.80665f, Mpu9150::scaleAccel(16384));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, Mpu9150::scaleGyro(131));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 36.53f, Mpu9150::scaleTemp(0));
}
