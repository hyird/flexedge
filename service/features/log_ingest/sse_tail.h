#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Streaming.h>

#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"

namespace service::log_ingest {

template <typename TailData>
inline std::optional<std::string> tailResponseCursor(const TailData& data) {
    const auto& cursor = data.template get<"cursor">();
    if (!cursor) {
        return std::nullopt;
    }
    return std::string(cursor->view());
}

inline void advanceTailCursor(std::optional<TailCursor>& target,
                              const std::optional<std::string>& cursor) {
    if (!cursor) {
        return;
    }
    if (const auto parsed = parseTailCursor(*cursor)) {
        target = *parsed;
    }
}

// Emits an initial snapshot, then a replacement snapshot for each coalesced
// notification. The client therefore never needs a companion REST read.
template <typename Fetch, typename WriteEvent>
ruvia::Task<void> streamSseSnapshots(ruvia::Context& c, fanout::Hub::Subscription subscription,
                                     Fetch fetch, WriteEvent writeEvent) {
    try {
        auto initial = co_await fetch();
        auto events = c.streamSse();
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});
        co_await writeEvent(c, events, std::move(initial));

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
            auto snapshot = co_await fetch();
            co_await writeEvent(c, events, std::move(snapshot));
        }
    } catch (const std::exception& error) {
        if (sseClientDisconnected(error)) {
            co_return;
        }
        throw;
    }
}

template <typename Fetch, typename CursorValue, typename WriteEvent>
ruvia::Task<void> streamSseTail(ruvia::Context& c, fanout::Hub::Subscription subscription,
                                std::optional<TailCursor> after, Fetch fetch,
                                CursorValue cursorValue, WriteEvent writeEvent) {
    try {
        auto initial = co_await fetch(after);
        const auto initialCursor = cursorValue(initial);
        advanceTailCursor(after, initialCursor);

        auto events = c.streamSse();
        co_await events.write(
            {.data = "{}", .event = "ready", .retry = std::chrono::milliseconds{3000}});
        co_await writeEvent(c, events, std::move(initial), initialCursor);

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

            auto update = co_await fetch(after);
            const auto updateCursor = cursorValue(update);
            if (!updateCursor) {
                continue;
            }
            advanceTailCursor(after, updateCursor);
            co_await writeEvent(c, events, std::move(update), updateCursor);
        }
    } catch (const std::exception& error) {
        if (sseClientDisconnected(error)) {
            co_return;
        }
        throw;
    }
}

} // namespace service::log_ingest
