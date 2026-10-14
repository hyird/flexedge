#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/context.h>
#include <ruvia/web/streaming.h>

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/connection_error.h"
#include "service/domains/auth/auth_session_read_service.h"
#include "service/domains/auth/session_credential.h"
#include "service/features/live_resource/read_scope.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/tail.h"
#include "service/middleware/auth.h"

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
        auto events = c.stream_sse();
        auto credential = std::make_shared<std::optional<service::auth::session_credential>>(
            service::auth::read_session_cookie(c));
        auto checked_at = std::chrono::steady_clock::now();
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
            if (std::chrono::steady_clock::now() - checked_at >= fanout::sse_heartbeat_interval) {
                const auto principal = co_await service::live_resource::read_once<std::optional<service::auth::authenticated_principal>>(
                    c, [credential](auto& read) -> ruvia::task<std::optional<service::auth::authenticated_principal>> {
                        if (!*credential) {
                            co_return std::nullopt;
                        }
                        co_return co_await service::auth::resolve_session_principal(read.db(), **credential);
                    });
                if (!principal || principal->system_tenant_id_ != service::middleware::current_tenant_id(c)) {
                    co_await events.write({.data_ = "{}", .event_ = "session-expired"});
                    co_return;
                }
                checked_at = std::chrono::steady_clock::now();
            }
            const auto signal =
                co_await subscription.receive_for(fanout::sse_heartbeat_interval, c.get_stop_token());
            if (events.aborted()) {
                co_return;
            }
            if (!signal.has_value()) {
                if (signal.status() != ruvia::worker_wait_status::timed_out) {
                    co_return;
                }
                co_await events.write({.event_ = "heartbeat"});
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
