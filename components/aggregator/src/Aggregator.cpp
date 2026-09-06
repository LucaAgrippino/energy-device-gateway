#include "Aggregator.hpp"

#include "esp_timer.h"
#include "freertos/task.h"

Aggregator::Aggregator(std::vector<MailboxEntry> mailboxes, ISnapshotSink& publisher)
    : mailboxes_(std::move(mailboxes)), publisher_(publisher) {}

Snapshot Aggregator::collectSnapshot(int64_t now_us) {
    Snapshot snap;
    snap.timestamp = now_us;

    for (auto& entry : mailboxes_) {
        Reading reading;

        // Non-blocking peek — returns pdTRUE if mailbox has data. Peek (not
        // receive) so a source that hasn't produced a new reading since the
        // last cycle still reports its latest known value (DESIGN.md §2).
        if (xQueuePeek(entry.queue, &reading, 0) == pdTRUE) {
            // Staleness may only downgrade the producer's verdict, never
            // replace it. Assigning a freshness result straight into status
            // silently discarded any Status::ERROR a device had set: both
            // Modbus devices republish allFailed() with a *fresh* timestamp on
            // every failed poll, so the reading never looked stale and a dead
            // slave was reported as OK with a value of 0 — indistinguishable
            // from a healthy source genuinely reading zero (DESIGN.md §7).
            if (isStale(reading.timestamp, snap.timestamp, entry.timeout_us)) {
                reading.status = Reading::Status::TIMEOUT;
            }
        } else {
            // Mailbox empty — no data ever received from this source.
            reading.source = entry.name;
            reading.value = 0.0f;
            reading.timestamp = 0;
            reading.status = Reading::Status::TIMEOUT;
        }

        snap.readings.push_back(std::move(reading));
    }

    return snap;
}

// Returns a plain predicate rather than a Status. The previous signature
// returned OK/TIMEOUT, which made `status = checkStale(...)` the natural-looking
// call and was exactly how the producer's own status came to be overwritten.
bool Aggregator::isStale(int64_t reading_ts, int64_t now, int64_t timeout) {
    return now - reading_ts > timeout;
}

void Aggregator::run() {
    const TickType_t period = pdMS_TO_TICKS(CONFIG_AGGREGATOR_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();

    while (true) {
        Snapshot snap = collectSnapshot(esp_timer_get_time());
        publisher_.publish(std::move(snap));

        vTaskDelayUntil(&last_wake, period);
    }
}
