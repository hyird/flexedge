#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <asio/steady_timer.hpp>

#include "node/data/health_probe.h"
#include "node/data/origin_tls.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class origin_health_supervisor final : public std::enable_shared_from_this<origin_health_supervisor> {
public:
    static std::shared_ptr<origin_health_supervisor> create(
        ruvia::event_loop loop, runtime_state& runtime, origin_health_registry& registry,
        origin_tls_context& tls_context) {
        auto result = std::shared_ptr<origin_health_supervisor>(
            new origin_health_supervisor(std::move(loop), runtime, registry, tls_context));
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
        origin_health_registry& registry, origin_tls_context& tls_context)
        : loop_(std::move(loop)),
          runtime_(runtime),
          registry_(registry),
          timer_(loop_.io_context()),
          tls_context_(tls_context) {}

public:
    ~origin_health_supervisor() {
        if (started_ && !stop_and_wait_completed_) {
            stop();
        }
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
    void tick() {
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
                    if (!registry_.claim_due(
                            website->id(), origin.id(),
                            std::chrono::seconds(website->health_check_interval_seconds()))) {
                        continue;
                    }
                    auto probe = std::make_shared<origin_health_probe>(
                        loop_, registry_.get_target(website->id(), origin.id()), tls_context_,
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
    origin_health_registry& registry_;
    asio::steady_timer timer_;
    ruvia::event_loop_stop_registration stop_registration_;
    origin_tls_context& tls_context_;
    std::unordered_map<origin_health_probe*, std::shared_ptr<origin_health_probe>> probes_;
    std::vector<std::coroutine_handle<>> stop_waiters_;
    bool timer_pending_{};
    bool started_{};
    bool stopped_{};
};

}  // namespace flexedge::node
