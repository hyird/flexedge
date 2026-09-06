#pragma once

#include <chrono>
#include <exception>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Streaming.h>

#include "service/features/node_runtime/fanout.h"

namespace service::node_runtime {

inline bool sseClientDisconnected(const std::exception& error) {
    const std::string_view message{error.what()};
    return message.contains("Broken pipe") || message.contains("Connection reset by peer") ||
           message.contains("redis operation cancelled");
}

inline ruvia::Task<void> streamSseRuntime(ruvia::Context& c, fanout::Hub::Subscription subscription) {
    try {
        auto events = c.streamSse();
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});

        while (!events.aborted()) {
            const auto signal =
                co_await subscription.receiveFor(fanout::kSseHeartbeatInterval, c.stopToken());
            if (events.aborted()) {
                co_return;
            }
            if (!signal.hasValue()) {
                if (signal.status() != ruvia::WorkerWaitStatus::kTimedOut) {
                    co_return;
                }
                co_await events.write({.event = "heartbeat"});
                continue;
            }
            co_await events.write({.data = "{}", .event = "node-state"});
        }
    } catch (const std::exception& error) {
        if (sseClientDisconnected(error)) {
            co_return;
        }
        throw;
    }
}

} // namespace service::node_runtime
