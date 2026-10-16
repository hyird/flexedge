#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/steady_timer.hpp>

#include "node/data/health_probe.h"
#include "node/data/origin_health.h"
#include "node/data/origin_tls.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class origin_health_supervisor;

// Reports request outcomes of one origin membership from a data worker. Outcomes travel to the
// supervisor loop as messages; the worker never touches the health state itself.
class origin_health_target final {
public:
    origin_health_target() = default;

    void success(std::uint32_t threshold) const noexcept {
        record(true, threshold);
    }
    void failure(std::uint32_t threshold) const noexcept {
        record(false, threshold);
    }

private:
    friend class origin_health_supervisor;
    origin_health_target(std::weak_ptr<origin_health_supervisor> supervisor, origin_health_key key,
        std::uint64_t id)
        : supervisor_(std::move(supervisor)),
          key_(std::move(key)),
          id_(id) {}

    void record(bool success, std::uint32_t threshold) const noexcept;

    std::weak_ptr<origin_health_supervisor> supervisor_;
    origin_health_key key_;
    std::uint64_t id_{};
};

// Single owner of origin health: runs active probes, applies the outcomes data workers send
// and publishes an immutable origin_health_snapshot. All health state lives on its loop.
class origin_health_supervisor final : public std::enable_shared_from_this<origin_health_supervisor> {
public:
    static std::shared_ptr<origin_health_supervisor> create(
        ruvia::event_loop loop, runtime_state& runtime, origin_tls_context& tls_context) {
        auto result = std::shared_ptr<origin_health_supervisor>(
            new origin_health_supervisor(std::move(loop), runtime, tls_context));
        result->stop_registration_ = result->loop_.on_stop(
            [weak = result->weak_from_this()]() -> ruvia::task<void> {
                if (const auto self = weak.lock()) {
                    co_await self->stop_and_wait();
                }
                co_return;
            });
        return result;
    }

private:
    origin_health_supervisor(ruvia::event_loop loop, runtime_state& runtime,
        origin_tls_context& tls_context)
        : loop_(std::move(loop)),
          runtime_(runtime),
          timer_(loop_.io_context()),
          tls_context_(tls_context) {}

public:
    ~origin_health_supervisor() {
        if (started_ && !stop_and_wait_completed_) {
            stop();
        }
    }

    // Any thread.
    [[nodiscard]] std::shared_ptr<const origin_health_snapshot> snapshot() const noexcept {
        return published_.load(std::memory_order_acquire);
    }

    // Any thread. Empty when the origin is not configured.
    [[nodiscard]] origin_health_target target(std::string_view website_id,
        std::string_view origin_id) {
        const auto current = snapshot();
        const auto* origin = current->find(website_id, origin_id);
        if (origin == nullptr) {
            return {};
        }
        return origin_health_target(weak_from_this(),
            {.website_id_ = std::string(website_id), .origin_id_ = std::string(origin_id)},
            origin->id_);
    }

    // Owner loop. Builds the next membership without changing the published health.
    [[nodiscard]] origin_health_state::prepared prepare(const origin_health_key_set& keys) {
        if (!loop_.is_current()) {
            throw std::logic_error("origin health must be prepared on its owner loop");
        }
        return state_.prepare(keys);
    }

    // Owner loop.
    void activate(origin_health_state::prepared&& next) noexcept {
        auto next_snapshot = next.snapshot();
        state_.activate(std::move(next));
        published_.store(std::move(next_snapshot), std::memory_order_release);
        schedule_publish();
    }

    void request_start() {
        if (!loop_.post([self = shared_from_this()] {
                      if (self->started_ || self->stopped_) {
                          return;
                      }
                      self->started_ = true;
                      self->tick();
                  })
                .accepted()) {
            throw std::runtime_error("origin health worker is stopping");
        }
    }

    void request_stop() noexcept {
        std::lock_guard lock(lifecycle_mutex_);
        if (stop_and_wait_completed_) {
            return;
        }
        if (loop_.is_current()) {
            stop();
            return;
        }
        asio::post(loop_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->stop();
            }
        });
    }

private:
    struct stop_awaiter final {
        origin_health_supervisor* supervisor_;
        bool await_ready() const noexcept {
            return supervisor_->stop_complete();
        }
        void await_suspend(std::coroutine_handle<> continuation) {
            supervisor_->stop_waiters_.push_back(continuation);
        }
        void await_resume() const noexcept {}
    };

    bool stop_complete() const noexcept {
        return stopped_ && !timer_pending_ && probes_.empty();
    }

    void notify_stop_waiters() noexcept {
        if (!stop_complete()) {
            return;
        }
        {
            std::lock_guard lock(lifecycle_mutex_);
            stop_and_wait_completed_ = true;
        }
        auto waiters = std::move(stop_waiters_);
        stop_waiters_.clear();
        for (const auto waiter : waiters) {
            waiter.resume();
        }
    }

    void stop() noexcept {
        stopped_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
    }

public:
    ruvia::task<void> stop_and_wait() {
        {
            std::lock_guard lock(lifecycle_mutex_);
            if (stop_and_wait_completed_) {
                co_return;
            }
        }
        stop();
        std::vector<std::shared_ptr<origin_health_probe>> active;
        active.reserve(probes_.size());
        for (const auto& [_, probe] : probes_) {
            active.push_back(probe);
        }
        for (const auto& probe : active) {
            probe->cancel();
        }
        co_await stop_awaiter{this};
        std::lock_guard lock(lifecycle_mutex_);
        stop_and_wait_completed_ = true;
    }

private:
    friend class origin_health_target;

    // Any thread. An outcome that cannot change the published health is not sent; a snapshot
    // at most one owner-loop turn old can therefore drop an outcome that would only have
    // restarted a streak. Allocation failure drops the outcome.
    void record(const origin_health_key& key, std::uint64_t id, bool success,
        std::uint32_t threshold) noexcept {
        const auto current = snapshot();
        const auto* origin = current->find(key.website_id_, key.origin_id_);
        if (origin == nullptr || origin->id_ != id || origin->settled(success)) {
            return;
        }
        try {
            (void)loop_.post([weak = weak_from_this(), key, id, success, threshold] {
                const auto self = weak.lock();
                if (self && self->state_.record({key.website_id_, key.origin_id_}, id, success,
                                threshold)) {
                    self->schedule_publish();
                }
            });
        } catch (...) {
        }
    }

    // Owner loop. Coalesces every change made in one loop turn into one snapshot.
    void schedule_publish() noexcept {
        stale_ = true;
        if (publish_queued_) {
            return;
        }
        try {
            publish_queued_ = loop_.post([weak = weak_from_this()] {
                                       if (const auto self = weak.lock()) {
                                           self->publish_queued_ = false;
                                           self->publish();
                                       }
                                   })
                                  .accepted();
        } catch (...) {
            // The next tick publishes the stale state.
        }
    }

    void publish() noexcept {
        if (!stale_) {
            return;
        }
        try {
            published_.store(state_.snapshot(), std::memory_order_release);
            stale_ = false;
        } catch (...) {
            // The next tick retries.
        }
    }

    void tick() {
        publish();
        if (stopped_) {
            return;
        }
        const auto config = runtime_.config();
        if (config && config->enabled()) {
            for (const auto* website : config->websites()) {
                if (!website->enabled() || !website->health_check_enabled()) {
                    continue;
                }
                for (const auto& origin : website->origins()) {
                    if (!origin.enabled()) {
                        continue;
                    }
                    const origin_health_key_view key{website->id(), origin.id()};
                    if (!state_.claim_due(
                            key, std::chrono::seconds(website->health_check_interval_seconds()))) {
                        continue;
                    }
                    auto probe = std::make_shared<origin_health_probe>(
                        loop_, state_.probe_target(key), tls_context_,
                        origin_probe_config{
                            .protocol_ = origin.protocol(),
                            .host_ = origin.host(),
                            .port_ = static_cast<std::uint16_t>(origin.port()),
                            .path_ = website->health_check_path(),
                            .timeout_ =
                                std::chrono::seconds(website->health_check_timeout_seconds()),
                            .expected_status_ = website->health_check_expected_status() == 0
                                                    ? 200
                                                    : website->health_check_expected_status(),
                            .healthy_threshold_ = website->healthy_threshold(),
                            .unhealthy_threshold_ = website->unhealthy_threshold(),
                        });
                    probes_.emplace(probe.get(), probe);
                    probe->set_on_retired([weak = weak_from_this(), identity = probe.get()] {
                        if (const auto self = weak.lock()) {
                            self->probes_.erase(identity);
                            self->schedule_publish();
                            self->notify_stop_waiters();
                        }
                    });
                    probe->start();
                }
            }
        }
        try {
            timer_.expires_after(std::chrono::seconds(1));
            timer_pending_ = true;
            timer_.async_wait([weak = weak_from_this()](const std::error_code& error) {
                if (const auto self = weak.lock()) {
                    self->timer_pending_ = false;
                    if (!error) {
                        self->tick();
                    }
                    self->notify_stop_waiters();
                }
            });
        } catch (...) {
            timer_pending_ = false;
            stopped_ = true;
            std::vector<std::shared_ptr<origin_health_probe>> active;
            active.reserve(probes_.size());
            for (const auto& [_, probe] : probes_) {
                active.push_back(probe);
            }
            for (const auto& probe : active) {
                probe->cancel();
            }
            notify_stop_waiters();
        }
    }

    ruvia::event_loop loop_;
    std::mutex lifecycle_mutex_;
    bool stop_and_wait_completed_{};
    runtime_state& runtime_;
    origin_health_state state_;
    std::atomic<std::shared_ptr<const origin_health_snapshot>> published_{
        std::make_shared<const origin_health_snapshot>()};
    asio::steady_timer timer_;
    ruvia::event_loop_stop_registration stop_registration_;
    origin_tls_context& tls_context_;
    std::unordered_map<origin_health_probe*, std::shared_ptr<origin_health_probe>> probes_;
    std::vector<std::coroutine_handle<>> stop_waiters_;
    bool timer_pending_{};
    bool started_{};
    bool stopped_{};
    bool stale_{};
    bool publish_queued_{};
};

inline void origin_health_target::record(bool success, std::uint32_t threshold) const noexcept {
    if (const auto supervisor = supervisor_.lock()) {
        supervisor->record(key_, id_, success, threshold);
    }
}

}  // namespace flexedge::node
