#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/context.h>
#include <ruvia/web/streaming.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include "service/common/connection_error.h"
#include "service/features/live_resource/read_scope.h"
#include "service/features/live_resource/sse.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"

namespace service::log_ingest {

struct tail_batch final {
    std::string payload_;
    std::optional<std::string> cursor_;
};

template <typename tail_data_type>
inline std::optional<std::string> tail_response_cursor(const tail_data_type& data) {
    const auto& cursor = data.template get<"cursor">();
    if (!cursor) {
        return std::nullopt;
    }
    return std::string(cursor->view());
}

inline void advance_tail_cursor(std::optional<tail_cursor>& target,
    const std::optional<std::string>& cursor) {
    if (!cursor) {
        return;
    }
    if (const auto parsed = parse_tail_cursor(*cursor)) {
        target = *parsed;
    }
}

template <typename fetch_type>
ruvia::task<void> stream_sse_tail(ruvia::context& c, fanout::hub::subscription subscription,
    std::optional<tail_cursor> after, fetch_type fetch) {
    try {
        service::live_resource::sse_connection events(c);
        auto initial = co_await service::live_resource::read_once<tail_batch>(c,
            [fetch, after](auto& read) { return fetch(read, after); });
        const auto initial_cursor = initial.cursor_;
        advance_tail_cursor(after, initial_cursor);

        co_await events.write(
            {.data_ = "{}", .event_ = "ready", .retry_ = std::chrono::milliseconds{3000}});
        ruvia::sse_message initial_message{.data_ = initial.payload_, .event_ = "logs"};
        if (initial_cursor) {
            initial_message.id_ = *initial_cursor;
        }
        co_await events.write(initial_message);

        while (!events.aborted()) {
            if (!co_await events.session_valid()) {
                co_return;
            }
            const auto signal = co_await subscription.receive_for(
                std::max(events.next_wake_at() - service::live_resource::sse_connection::clock::now(),
                    service::live_resource::sse_connection::clock::duration::zero()),
                c.get_stop_token());
            if (events.aborted()) {
                co_return;
            }
            if (!signal.has_value()) {
                if (signal.status() != ruvia::worker_wait_status::timed_out) {
                    co_return;
                }
                co_await events.keep_alive();
                continue;
            }

            auto update = co_await service::live_resource::read_once<tail_batch>(c,
                [fetch, after](auto& read) { return fetch(read, after); });
            const auto update_cursor = update.cursor_;
            if (!update_cursor) {
                continue;
            }
            advance_tail_cursor(after, update_cursor);
            co_await events.write({.data_ = update.payload_, .event_ = "logs", .id_ = *update_cursor});
        }
    } catch (const std::exception& error) {
        if (service::common::sse_client_disconnected(error)) {
            co_return;
        }
        throw;
    }
}

}  // namespace service::log_ingest
