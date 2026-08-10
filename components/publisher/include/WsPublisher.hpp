#pragma once

#include <condition_variable>
#include <mutex>
#include <string>

#include "ISnapshotSink.hpp"
#include "Snapshot.hpp"
#include "esp_http_server.h"

class WsPublisher : public ISnapshotSink {
public:
    WsPublisher() = default;
    ~WsPublisher() override;

    WsPublisher(const WsPublisher&) = delete;
    WsPublisher& operator=(const WsPublisher&) = delete;
    WsPublisher(WsPublisher&&) = delete;
    WsPublisher& operator=(WsPublisher&&) = delete;

    esp_err_t start();
    void stop();

    // Called by AggregatorTask (producer side). Takes ownership of snapshot —
    // Reading is move-only (CLAUDE.md), so Snapshot can't be copy-assigned;
    // the aggregator must give up its snapshot on each publish() call.
    void publish(Snapshot&& snapshot) override;

    // Called by PublisherTask (consumer side). Never returns.
    void run();

    [[nodiscard]] httpd_handle_t serverHandle() const { return server_; }

    // Pure JSON formatting, exposed for host-independent unit testing
    // (test/test_ws_publisher.cpp, DESIGN.md §12), independent of the httpd runtime.
    static std::string snapshotToJson(const Snapshot& snap);

private:
    httpd_handle_t server_{nullptr};
    Snapshot latest_snapshot_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool data_ready_{false};

    static esp_err_t wsHandler(httpd_req_t* req);
    static esp_err_t dashboardHandler(httpd_req_t* req);
    void broadcastJson(const std::string& json);
};
