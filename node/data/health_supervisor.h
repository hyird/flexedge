#pragma once

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

#include <ruvia/core/EventLoop.h>

#include "node/data/health_probe.h"
#include "node/data/origin_tls.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class OriginHealthSupervisor final : public std::enable_shared_from_this<OriginHealthSupervisor> {
  public:
    static std::shared_ptr<OriginHealthSupervisor> create(
        ruvia::EventLoop loop, RuntimeState& runtime, OriginHealthRegistry& registry,
        OriginTlsContext& tlsContext) {
        auto result = std::shared_ptr<OriginHealthSupervisor>(
            new OriginHealthSupervisor(std::move(loop), runtime, registry, tlsContext));
        result->stopRegistration_ = result->loop_.onStop(
            [weak = result->weak_from_this()]() -> ruvia::Task<void> {
                if (const auto self = weak.lock()) co_await self->stopAndWait();
                co_return;
            });
        return result;
    }

  private:
    OriginHealthSupervisor(ruvia::EventLoop loop, RuntimeState& runtime,
                           OriginHealthRegistry& registry, OriginTlsContext& tlsContext)
        : loop_(std::move(loop)), runtime_(runtime), registry_(registry), timer_(loop_.ioContext()),
          tlsContext_(tlsContext) {}

  public:
    ~OriginHealthSupervisor() {
        if (started_ && !stopAndWaitCompleted_) stop();
    }

    void requestStart() {
        if (!loop_.post([self = shared_from_this()] {
                if (self->started_ || self->stopped_) return;
                self->started_ = true;
                self->tick();
            }).accepted()) {
            throw std::runtime_error("origin health worker is stopping");
        }
    }

    void requestStop() noexcept {
        std::lock_guard lock(lifecycleMutex_);
        if (stopAndWaitCompleted_) return;
        if (loop_.isCurrent()) {
            stop();
            return;
        }
        asio::post(loop_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) self->stop();
        });
    }

  private:
    struct StopAwaiter final {
        OriginHealthSupervisor* supervisor;
        bool await_ready() const noexcept { return supervisor->stopComplete(); }
        void await_suspend(std::coroutine_handle<> continuation) {
            supervisor->stopWaiters_.push_back(continuation);
        }
        void await_resume() const noexcept {}
    };

    bool stopComplete() const noexcept {
        return stopped_ && !timerPending_ && probes_.empty();
    }

    void notifyStopWaiters() noexcept {
        if (!stopComplete()) return;
        {
            std::lock_guard lock(lifecycleMutex_);
            stopAndWaitCompleted_ = true;
        }
        auto waiters = std::move(stopWaiters_);
        stopWaiters_.clear();
        for (const auto waiter : waiters) waiter.resume();
    }

    void stop() noexcept {
        stopped_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
    }

  public:
    ruvia::Task<void> stopAndWait() {
        {
            std::lock_guard lock(lifecycleMutex_);
            if (stopAndWaitCompleted_) co_return;
        }
        stop();
        std::vector<std::shared_ptr<OriginHealthProbe>> active;
        active.reserve(probes_.size());
        for (const auto& [_, probe] : probes_) active.push_back(probe);
        for (const auto& probe : active) probe->cancel();
        co_await StopAwaiter{this};
        std::lock_guard lock(lifecycleMutex_);
        stopAndWaitCompleted_ = true;
    }

  private:
    void tick() {
        if (stopped_) return;
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
                    if (!registry_.claimDue(
                            website->id(), origin.id(),
                            std::chrono::seconds(website->health_check_interval_seconds()))) {
                        continue;
                    }
                    auto probe = std::make_shared<OriginHealthProbe>(
                        loop_, registry_.target(website->id(), origin.id()), tlsContext_,
                        OriginProbeConfig{
                            .protocol = origin.protocol(),
                            .host = origin.host(),
                            .port = static_cast<std::uint16_t>(origin.port()),
                            .path = website->health_check_path(),
                            .timeout =
                                std::chrono::seconds(website->health_check_timeout_seconds()),
                            .expectedStatus = website->health_check_expected_status() == 0
                                                  ? 200
                                                  : website->health_check_expected_status(),
                            .healthyThreshold = website->healthy_threshold(),
                            .unhealthyThreshold = website->unhealthy_threshold(),
                        });
                    probes_.emplace(probe.get(), probe);
                    probe->setOnRetired([weak = weak_from_this(), identity = probe.get()] {
                        if (const auto self = weak.lock()) {
                            self->probes_.erase(identity);
                            self->notifyStopWaiters();
                        }
                    });
                    probe->start();
                }
            }
        }
        try {
            timer_.expires_after(std::chrono::seconds(1));
            timerPending_ = true;
            timer_.async_wait([weak = weak_from_this()](const std::error_code& error) {
                if (const auto self = weak.lock()) {
                    self->timerPending_ = false;
                    if (!error) self->tick();
                    self->notifyStopWaiters();
                }
            });
        } catch (...) {
            timerPending_ = false;
            stopped_ = true;
            std::vector<std::shared_ptr<OriginHealthProbe>> active;
            active.reserve(probes_.size());
            for (const auto& [_, probe] : probes_) active.push_back(probe);
            for (const auto& probe : active) probe->cancel();
            notifyStopWaiters();
        }
    }

    ruvia::EventLoop loop_;
    std::mutex lifecycleMutex_;
    bool stopAndWaitCompleted_{};
    RuntimeState& runtime_;
    OriginHealthRegistry& registry_;
    asio::steady_timer timer_;
    ruvia::EventLoopStopRegistration stopRegistration_;
    OriginTlsContext& tlsContext_;
    std::unordered_map<OriginHealthProbe*, std::shared_ptr<OriginHealthProbe>> probes_;
    std::vector<std::coroutine_handle<>> stopWaiters_;
    bool timerPending_{};
    bool started_{};
    bool stopped_{};
};

} // namespace flexedge::node
