#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

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
        result->stopRegistration_ = result->loop_.onStop([weak = result->weak_from_this()] {
            if (const auto self = weak.lock()) self->stop();
        });
        return result;
    }

  private:
    OriginHealthSupervisor(ruvia::EventLoop loop, RuntimeState& runtime,
                           OriginHealthRegistry& registry, OriginTlsContext& tlsContext)
        : loop_(std::move(loop)), runtime_(runtime), registry_(registry), timer_(loop_.ioContext()),
          tlsContext_(tlsContext) {}

  public:
    ~OriginHealthSupervisor() { stop(); }

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
        if (loop_.isCurrent()) {
            stop();
            return;
        }
        asio::post(loop_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) self->stop();
        });
    }

  private:
    void stop() noexcept {
        stopped_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
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
                    std::make_shared<OriginHealthProbe>(
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
                        })
                        ->start();
                }
            }
        }
        timer_.expires_after(std::chrono::seconds(1));
        timer_.async_wait([weak = weak_from_this()](const std::error_code& error) {
            if (!error) {
                if (const auto self = weak.lock()) self->tick();
            }
        });
    }

    ruvia::EventLoop loop_;
    RuntimeState& runtime_;
    OriginHealthRegistry& registry_;
    asio::steady_timer timer_;
    ruvia::EventLoopStopRegistration stopRegistration_;
    OriginTlsContext& tlsContext_;
    bool started_{};
    bool stopped_{};
};

} // namespace flexedge::node
