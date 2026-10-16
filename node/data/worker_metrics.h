#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "node/runtime/connected_clients.h"
#include "node/runtime/log_producer.h"

namespace flexedge::node {

// Counters of one data worker. Only the owning worker loop reads or writes them; other loops
// receive immutable snapshots from their data_worker or local drain notifications.
class worker_metrics final {
public:
    void connection_opened() noexcept {
        ++connections_;
    }

    void connection_closed() noexcept {
        if (--connections_ == 0) {
            notify_drained();
        }
    }

    void traffic_out(std::size_t bytes) noexcept {
        traffic_out_bytes_ += bytes;
    }

    [[nodiscard]] std::int64_t connection_count() const noexcept {
        return connections_;
    }

    [[nodiscard]] std::uint64_t traffic_out_bytes() const noexcept {
        return traffic_out_bytes_;
    }

    // Sends once the worker has no open connection: now, or when its last connection closes.
    void notify_when_drained(ruvia::channel_sender<bool> waiter) {
        if (connections_ == 0) {
            (void)waiter.send(true);
            return;
        }
        drain_waiters_.push_back(std::move(waiter));
    }

private:
    void notify_drained() noexcept {
        auto waiters = std::move(drain_waiters_);
        drain_waiters_.clear();
        for (const auto& waiter : waiters) {
            try {
                (void)waiter.send(true);
            } catch (...) {
                // A waiter that cannot be told times out on its own deadline.
            }
        }
    }

    std::int64_t connections_{};
    std::uint64_t traffic_out_bytes_{};
    std::vector<ruvia::channel_sender<bool>> drain_waiters_;
};

// Load of all data workers at one moment.
struct data_plane_load final {
    std::int64_t connection_count_{};
    std::uint64_t traffic_out_bytes_{};
    // Distinct client IP addresses with an open connection on any worker.
    std::int64_t connected_ip_count_{};
    log_production_snapshot logs_{};
};

inline constexpr auto worker_report_timeout = std::chrono::seconds(2);

}  // namespace flexedge::node
