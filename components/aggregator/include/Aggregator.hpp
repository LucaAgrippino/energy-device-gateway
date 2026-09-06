#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ISnapshotSink.hpp"
#include "Reading.hpp"
#include "Snapshot.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

struct MailboxEntry {
    QueueHandle_t    queue;        // The FreeRTOS mailbox
    std::string_view name;         // Source name for logging
    int64_t          timeout_us;   // Stale threshold in µs
};

class Aggregator {
public:
    // Depends on ISnapshotSink rather than WsPublisher directly — WsPublisher
    // pulls in esp_http_server, which has no "linux" target port, and this
    // component is meant to be host-testable (DESIGN.md §9).
    Aggregator(std::vector<MailboxEntry> mailboxes, ISnapshotSink& publisher);
    ~Aggregator() = default;

    Aggregator(const Aggregator&) = delete;
    Aggregator& operator=(const Aggregator&) = delete;
    Aggregator(Aggregator&&) = delete;
    Aggregator& operator=(Aggregator&&) = delete;

    void run();   // Called by AggregatorTask — loops forever

    // Pure logic, exposed for host-based unit testing (DESIGN.md §9),
    // independent of the FreeRTOS task loop. now_us is injected rather than
    // read via esp_timer_get_time() internally, since esp_timer likewise has
    // no linked implementation on the "linux" target (header-only there).
    Snapshot collectSnapshot(int64_t now_us);

    // A predicate, not a Status: staleness is only one input to a reading's
    // final status, which must also respect the error the producer reported
    // (DESIGN.md §7).
    [[nodiscard]] static bool isStale(int64_t reading_ts, int64_t now, int64_t timeout);

private:
    std::vector<MailboxEntry> mailboxes_;
    ISnapshotSink& publisher_;
};
