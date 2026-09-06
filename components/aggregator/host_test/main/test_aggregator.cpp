#include <cstdlib>
#include <string>

#include "Aggregator.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "unity.h"
#include "unity_test_runner.h"

namespace {

class MockSink : public ISnapshotSink {
public:
    void publish(Snapshot&& snapshot) override {
        last = std::move(snapshot);
        called = true;
    }
    Snapshot last;
    bool called = false;
};

QueueHandle_t makeMailbox() {
    return xQueueCreate(1, sizeof(Reading));
}

}  // namespace

TEST_CASE("Aggregator: fresh reading reports OK", "[aggregator]") {
    QueueHandle_t q = makeMailbox();
    Reading r{"imu", 1.5f, 1000, Reading::Status::OK};
    xQueueOverwrite(q, &r);

    MockSink sink;
    Aggregator agg({{q, "imu", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(1200);  // now - reading_ts = 200, within the 500 timeout

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::OK),
                           static_cast<int>(snap.readings[0].status));
    TEST_ASSERT_EQUAL_FLOAT(1.5f, snap.readings[0].value);

    vQueueDelete(q);
}

TEST_CASE("Aggregator: stale reading reports TIMEOUT", "[aggregator]") {
    QueueHandle_t q = makeMailbox();
    Reading r{"imu", 1.5f, 1000, Reading::Status::OK};
    xQueueOverwrite(q, &r);

    MockSink sink;
    Aggregator agg({{q, "imu", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(2000);  // now - reading_ts = 1000, exceeds the 500 timeout

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::TIMEOUT),
                           static_cast<int>(snap.readings[0].status));

    vQueueDelete(q);
}

// Regression: a device that diagnoses its own failure republishes on every poll
// with a *fresh* timestamp, so it never looks stale. The aggregator used to
// assign the freshness verdict straight into status, relabelling that ERROR as
// OK — a dead Modbus slave then reached the dashboard as 0.000 with status OK,
// indistinguishable from a healthy source reading zero.
TEST_CASE("Aggregator: fresh reading keeps a producer-reported ERROR", "[aggregator]") {
    QueueHandle_t q = makeMailbox();
    Reading r{"modbus_tcp", 0.0f, 1000, Reading::Status::ERROR};
    xQueueOverwrite(q, &r);

    MockSink sink;
    Aggregator agg({{q, "modbus_tcp", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(1200);  // fresh: within the 500 timeout

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::ERROR),
                           static_cast<int>(snap.readings[0].status));

    vQueueDelete(q);
}

TEST_CASE("Aggregator: fresh reading keeps a producer-reported TIMEOUT", "[aggregator]") {
    // ModbusTcpTask publishes TIMEOUT while Wi-Fi is down, with a current
    // timestamp. That must survive too, not be promoted to OK.
    QueueHandle_t q = makeMailbox();
    Reading r{"modbus_tcp", 0.0f, 1000, Reading::Status::TIMEOUT};
    xQueueOverwrite(q, &r);

    MockSink sink;
    Aggregator agg({{q, "modbus_tcp", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(1200);

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::TIMEOUT),
                           static_cast<int>(snap.readings[0].status));

    vQueueDelete(q);
}

TEST_CASE("Aggregator: staleness downgrades an otherwise-OK reading only", "[aggregator]") {
    // Staleness wins over the producer's OK, and an ERROR that has also gone
    // stale reports TIMEOUT — silence is the more accurate description once a
    // source stops publishing entirely.
    QueueHandle_t q = makeMailbox();
    Reading r{"modbus_rtu", 3.0f, 1000, Reading::Status::ERROR};
    xQueueOverwrite(q, &r);

    MockSink sink;
    Aggregator agg({{q, "modbus_rtu", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(2000);  // stale: exceeds the 500 timeout

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::TIMEOUT),
                           static_cast<int>(snap.readings[0].status));

    vQueueDelete(q);
}

TEST_CASE("Aggregator: isStale is a boundary-inclusive predicate", "[aggregator]") {
    // Exactly at the threshold is still fresh; one microsecond past is not.
    TEST_ASSERT_FALSE(Aggregator::isStale(1000, 1500, 500));
    TEST_ASSERT_TRUE(Aggregator::isStale(1000, 1501, 500));
}

TEST_CASE("Aggregator: empty mailbox reports TIMEOUT with value 0", "[aggregator]") {
    QueueHandle_t q = makeMailbox();  // never written to

    MockSink sink;
    Aggregator agg({{q, "modbus_rtu", 3000}}, sink);

    Snapshot snap = agg.collectSnapshot(5000);

    TEST_ASSERT_EQUAL(1, snap.readings.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Reading::Status::TIMEOUT),
                           static_cast<int>(snap.readings[0].status));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, snap.readings[0].value);
    TEST_ASSERT_EQUAL_STRING("modbus_rtu", std::string(snap.readings[0].source).c_str());

    vQueueDelete(q);
}

TEST_CASE("Aggregator: multiple sources produce readings in registration order", "[aggregator]") {
    QueueHandle_t q1 = makeMailbox();
    QueueHandle_t q2 = makeMailbox();
    QueueHandle_t q3 = makeMailbox();

    Reading r1{"imu", 1.0f, 100, Reading::Status::OK};
    Reading r2{"modbus_rtu", 2.0f, 100, Reading::Status::OK};
    xQueueOverwrite(q1, &r1);
    xQueueOverwrite(q2, &r2);
    // q3 left empty

    MockSink sink;
    Aggregator agg({{q1, "imu", 500}, {q2, "modbus_rtu", 3000}, {q3, "modbus_tcp", 5000}}, sink);

    Snapshot snap = agg.collectSnapshot(100);

    TEST_ASSERT_EQUAL(3, snap.readings.size());
    TEST_ASSERT_EQUAL_STRING("imu", std::string(snap.readings[0].source).c_str());
    TEST_ASSERT_EQUAL_STRING("modbus_rtu", std::string(snap.readings[1].source).c_str());
    TEST_ASSERT_EQUAL_STRING("modbus_tcp", std::string(snap.readings[2].source).c_str());

    vQueueDelete(q1);
    vQueueDelete(q2);
    vQueueDelete(q3);
}

// DESIGN.md §9's original intent ("verify snapshot timestamp is close to
// esp_timer_get_time()") doesn't apply as literally written — esp_timer has
// no linked implementation on the "linux" target, which is why
// collectSnapshot() takes now_us as a parameter instead of calling it
// internally (see Aggregator.hpp). This verifies that injected time flows
// through untouched instead.
TEST_CASE("Aggregator: snapshot timestamp echoes the injected now_us", "[aggregator]") {
    QueueHandle_t q = makeMailbox();
    MockSink sink;
    Aggregator agg({{q, "imu", 500}}, sink);

    Snapshot snap = agg.collectSnapshot(123456789);

    TEST_ASSERT_EQUAL_INT64(123456789, snap.timestamp);

    vQueueDelete(q);
}

extern "C" void app_main(void) {
    // unity_run_all_tests() alone never returns control to a process that
    // exits: app_main() returning just self-deletes the "main" FreeRTOS task
    // (freertos/app_startup.c), while the linux port's vTaskStartScheduler()
    // keeps running the idle task forever in this same process — there's no
    // ESP-IDF-side equivalent of "stop the scheduler and return from main()"
    // for the linux target. UNITY_BEGIN()/UNITY_END() also aren't called by
    // unity_run_all_tests() itself (only unity_run_menu(), the on-target
    // interactive runner, does that) so they're wrapped explicitly here to
    // get the usual pass/fail summary and a real process exit code.
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = UNITY_END();
    exit(failures);
}
