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
// receive immutable copies (collect_worker_load) or a notification (wait_workers_drained).
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

// One data worker loop and the counters it owns.
struct worker_counters final {
    ruvia::event_loop loop_;
    worker_metrics* metrics_{};
    const connected_clients* clients_{};
    const log_producer* logs_{};
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

// Runs on `owner`. Each worker copies its own counters on its loop and sends the immutable copy
// here. SO_REUSEPORT spreads one client's connections across workers, so per-worker IP counts
// cannot be added; the union of the address sets is counted. Empty when a worker cannot
// answer, so callers omit the load instead of reporting a partial one.
inline ruvia::task<std::optional<data_plane_load>> collect_worker_load(ruvia::worker_handle owner,
    std::span<const worker_counters> workers, ruvia::stop_token stop) {
    if (workers.empty()) {
        co_return data_plane_load{};
    }
    struct worker_load final {
        std::int64_t connection_count_{};
        std::uint64_t traffic_out_bytes_{};
        std::vector<connected_clients::address_key> addresses_;
        log_production_snapshot logs_{};
    };
    auto [sender, receiver] =
        ruvia::make_channel<worker_load>(std::move(owner), {.capacity_ = workers.size()});
    for (const auto& worker : workers) {
        const auto posted = worker.loop_.post(
            [sender, metrics = worker.metrics_, clients = worker.clients_, logs = worker.logs_] {
                try {
                    (void)sender.send({.connection_count_ = metrics->connection_count(),
                        .traffic_out_bytes_ = metrics->traffic_out_bytes(),
                        .addresses_ = clients->snapshot(),
                        .logs_ = logs ? logs->snapshot() : log_production_snapshot{}});
                } catch (...) {
                    // Without this worker's copy no load is reported; closing ends the wait now.
                    sender.close();
                }
            });
        if (!posted.accepted()) {
            co_return std::nullopt;
        }
    }
    data_plane_load result;
    std::vector<connected_clients::address_key> addresses;
    for (std::size_t received = 0; received < workers.size(); ++received) {
        auto copy = co_await receiver.receive_for(worker_report_timeout, stop);
        if (!copy.has_value()) {
            co_return std::nullopt;
        }
        const auto& load = copy.value();
        result.connection_count_ += load.connection_count_;
        result.traffic_out_bytes_ += load.traffic_out_bytes_;
        result.logs_.published_events_ += load.logs_.published_events_;
        result.logs_.queued_events_ += load.logs_.queued_events_;
        result.logs_.dropped_events_ += load.logs_.dropped_events_;
        addresses.insert(addresses.end(), load.addresses_.begin(), load.addresses_.end());
    }
    std::ranges::sort(addresses);
    const auto duplicates = std::ranges::unique(addresses);
    addresses.erase(duplicates.begin(), duplicates.end());
    result.connected_ip_count_ = static_cast<std::int64_t>(addresses.size());
    co_return result;
}

// Runs on `owner`. Completes with true once every worker has reported that its last connection
// closed, or with false when `timeout` elapses or a worker cannot be asked.
inline ruvia::task<bool> wait_workers_drained(ruvia::worker_handle owner,
    std::span<const worker_counters> workers, std::chrono::steady_clock::duration timeout) {
    if (workers.empty()) {
        co_return true;
    }
    auto [sender, receiver] =
        ruvia::make_channel<bool>(std::move(owner), {.capacity_ = workers.size()});
    for (const auto& worker : workers) {
        const auto posted = worker.loop_.post([sender, metrics = worker.metrics_] {
            try {
                metrics->notify_when_drained(sender);
            } catch (...) {
                sender.close();
            }
        });
        if (!posted.accepted()) {
            co_return false;
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (std::size_t received = 0; received < workers.size(); ++received) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= std::chrono::steady_clock::duration::zero()) {
            co_return false;
        }
        const auto drained = co_await receiver.receive_for(remaining);
        if (!drained.has_value()) {
            co_return false;
        }
    }
    co_return true;
}

}  // namespace flexedge::node
