#include "WsPublisher.hpp"
#include "unity.h"

// These cases exercise WsPublisher::snapshotToJson directly (DESIGN.md §8),
// independent of the httpd runtime — no WebSocket connection required.

TEST_CASE("snapshotToJson: empty snapshot produces an empty readings array", "[publisher]") {
    Snapshot snap;
    snap.timestamp = 1234567890;

    std::string json = WsPublisher::snapshotToJson(snap);

    TEST_ASSERT_EQUAL_STRING("{\"ts\":1234567890,\"readings\":[]}", json.c_str());
}

TEST_CASE("snapshotToJson: single reading formats src/val/st fields", "[publisher]") {
    Snapshot snap;
    snap.timestamp = 42;
    snap.readings.push_back(Reading{"imu.accel_z", 9.810f, 42, Reading::Status::OK});

    std::string json = WsPublisher::snapshotToJson(snap);

    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":42,\"readings\":[{\"src\":\"imu.accel_z\",\"val\":9.810,\"st\":0}]}",
        json.c_str());
}

TEST_CASE("snapshotToJson: multiple readings are comma-separated", "[publisher]") {
    Snapshot snap;
    snap.timestamp = 1;
    snap.readings.push_back(Reading{"modbus_rtu.voltage", 48.5f, 1, Reading::Status::OK});
    snap.readings.push_back(Reading{"modbus_tcp.power", 1200.0f, 1, Reading::Status::TIMEOUT});

    std::string json = WsPublisher::snapshotToJson(snap);

    TEST_ASSERT_EQUAL_STRING(
        "{\"ts\":1,\"readings\":["
        "{\"src\":\"modbus_rtu.voltage\",\"val\":48.500,\"st\":0},"
        "{\"src\":\"modbus_tcp.power\",\"val\":1200.000,\"st\":1}]}",
        json.c_str());
}
