#pragma once

#include "Snapshot.hpp"

// Decouples Aggregator from WsPublisher's concrete esp_http_server dependency
// so Aggregator.hpp stays host-testable (aggregator_DESIGN.md §9) — the real
// esp_http_server component has no "linux" target support, unlike freertos
// and esp_timer, so anything that transitively includes WsPublisher.hpp
// cannot build for the host_test target.
class ISnapshotSink {
public:
    virtual ~ISnapshotSink() = default;
    virtual void publish(Snapshot&& snapshot) = 0;
};
