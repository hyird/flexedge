#pragma once

#include <ruvia/core/move_only_function.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/context.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/streaming.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/connection_error.h"
#include "service/common/http.h"
#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/read_scope.h"
#include "service/features/live_resource/read_snapshot.h"
#include "service/middleware/auth.h"

namespace service::live_resource {

// One projection carried by a snapshot stream. Parts are paired by index with
// the parts of the stream's hub subscription: each has its own event source,
// cache key and single-flight read, and is re-read only when it changes.
//
// An incremental part (with `update_`, subscribed with `task_attempts_`)
// writes its snapshot once per connection and afterwards only the events
// `update_` builds from the task attempt notices of its mailbox; other
// changes of its resource never re-read it. It resends its snapshot (the
// recovery snapshot) after a history reset, a notice overflow, a missed
// notification, or when `update_` fails.
struct snapshot_part final {
    std::string_view event_;
    ruvia::move_only_function<ruvia::task<std::string>(snapshot_read_scope&)> fetch_;
    std::string_view update_event_{};
    ruvia::move_only_function<ruvia::task<std::vector<std::string>>(snapshot_read_scope&,
        std::vector<task_attempt_notice>)>
        update_{};
};

// Streams one or more projections over one SSE connection. Each part writes
// its initial snapshot once, then only snapshots whose bytes changed (or, for
// incremental parts, its update events). Read failures are `resource-error`
// frames; runtime patches follow the part's baseline. Coalesced parts are
// re-read when their deferral elapses.
inline ruvia::task<void> stream_snapshots(ruvia::context& c, hub::subscription subscription,
    std::vector<snapshot_part> parts) {
    using clock = std::chrono::steady_clock;
    if (parts.empty() || parts.size() != subscription.parts()) {
        throw std::logic_error("snapshot parts must pair with subscription parts");
    }
    // Business errors are SSE frames as well, including errors in the initial
    // snapshot. EventSource cannot inspect HTTP error bodies.
    auto events = c.stream_sse();
    const auto credential = std::make_shared<std::optional<service::auth::session_credential>>(
        service::auth::read_session_cookie(c));
    auto checked_at = clock::now();
    struct part_state final {
        // What this connection last wrote for the part; the cache may drop
        // and re-read the snapshot any number of times in between.
        written_snapshot written_;
        bool initial_ = true;
        pending_changes pending_{.snapshot_ = true,
            .runtime_ = {},
            .origins_ = {},
            .task_attempts_ = {},
            .history_reset_ = false,
            .recovery_ = false,
            .refresh_at_ = {}};
        std::optional<clock::time_point> refresh_at_;
    };
    std::vector<part_state> states(parts.size());
    try {
        co_await events.write(
            {.data_ = "{}", .event_ = "ready", .retry_ = std::chrono::milliseconds(3000)});
        auto heartbeat_at = clock::now() + heartbeat_interval;
        while (!events.aborted()) {
            const auto now = clock::now();
            if (now - checked_at >= heartbeat_interval) {
                // This bounded session check is security validation, never a
                // resource reload. Revoked sessions cannot retain live access.
                const auto principal =
                    co_await read_once<std::optional<service::auth::authenticated_principal>>(
                        c,
                        [credential](auto& read)
                            -> ruvia::task<std::optional<service::auth::authenticated_principal>> {
                            if (!*credential) {
                                co_return std::nullopt;
                            }
                            co_return co_await service::auth::resolve_session_principal(read.db(),
                                **credential);
                        });
                if (!principal ||
                    principal->system_tenant_id_ != service::middleware::current_tenant_id(c)) {
                    co_await events.write({.data_ = "{}", .event_ = "session-expired"});
                    co_return;
                }
                checked_at = now;
            }
            for (std::size_t index = 0; index < parts.size(); ++index) {
                auto& part = parts[index];
                auto& state = states[index];
                const bool incremental = static_cast<bool>(part.update_);
                bool resend = incremental ? state.initial_ || state.pending_.history_reset_
                                          : state.pending_.snapshot_;
                if (incremental && !resend && !state.pending_.task_attempts_.empty()) {
                    std::optional<std::vector<std::string>> frames;
                    try {
                        frames = co_await read_once<std::vector<std::string>>(c,
                            [&part, notices = std::move(state.pending_.task_attempts_)](
                                snapshot_read_scope& read) mutable { return part.update_(read, std::move(notices)); });
                    } catch (const std::exception& error) {
                        if (events.aborted() || c.get_stop_token().stop_requested()) {
                            co_return;
                        }
                        service::logging::error("Live resource update failure: " + std::string(error.what()));
                    }
                    if (frames) {
                        for (const auto& frame : *frames) {
                            co_await events.write({.data_ = frame, .event_ = part.update_event_});
                        }
                    } else {
                        resend = true;
                    }
                }
                if (resend) {
                    std::shared_ptr<const snapshot> snapshot_value;
                    try {
                        snapshot_value = co_await read_snapshot(
                            c, subscription.snapshots(index),
                            [&part](snapshot_read_scope& read) { return part.fetch_(read); });
                    } catch (const snapshot_capacity_error&) {
                        snapshot_value = std::make_shared<const snapshot>(snapshot_unavailable());
                    }
                    if (snapshot_value->failure_) {
                        co_await events.write({.data_ = snapshot_value->data_, .event_ = "resource-error"});
                        state.written_.forget();
                    } else {
                        // Only changed bytes are written. The initial and the
                        // recovery snapshot are always written: the client may
                        // have lost state. Increments moved the client away from
                        // the previous snapshot, so an incremental part resends.
                        if (state.initial_ || incremental || state.pending_.recovery_) {
                            state.written_.forget();
                        }
                        if (state.written_.replace(snapshot_value->data_)) {
                            co_await events.write({.data_ = snapshot_value->data_, .event_ = part.event_});
                        }
                    }
                    // A shared read may predate patches in this mailbox. Replay
                    // them after the baseline; revision/time guards in the client
                    // ignore patches older than the fetched lifecycle state.
                    state.initial_ = snapshot_value->failure_;
                }
                for (const auto& [id, runtime] : state.pending_.runtime_) {
                    (void)id;
                    co_await events.write({.data_ = runtime, .event_ = "node-runtime"});
                }
                for (const auto& [id, origins] : state.pending_.origins_) {
                    (void)id;
                    co_await events.write({.data_ = origins, .event_ = "origin-runtime"});
                }
            }
            // Wait for a change, a coalescing deadline, or the heartbeat.
            auto wake_at = heartbeat_at;
            for (const auto& state : states) {
                if (state.refresh_at_) {
                    wake_at = std::min(wake_at, *state.refresh_at_);
                }
            }
            const auto signal = co_await subscription.receive_for(
                std::max(wake_at - clock::now(), clock::duration::zero()), c.get_stop_token());
            if (events.aborted()) {
                co_return;
            }
            const auto woken_at = clock::now();
            if (signal.has_value()) {
                heartbeat_at = woken_at + heartbeat_interval;
            } else {
                if (signal.status() != ruvia::worker_wait_status::timed_out) {
                    co_return;
                }
                for (std::size_t index = 0; index < states.size(); ++index) {
                    auto& state = states[index];
                    if (state.refresh_at_ && woken_at >= *state.refresh_at_) {
                        state.refresh_at_.reset();
                        // Delivery marks this mailbox pending as well.
                        (void)subscription.release_coalesced(index, woken_at);
                    }
                }
                if (woken_at >= heartbeat_at) {
                    co_await events.write({.data_ = "{}", .event_ = "heartbeat"});
                    heartbeat_at = woken_at + heartbeat_interval;
                }
            }
            for (std::size_t index = 0; index < states.size(); ++index) {
                auto& state = states[index];
                state.pending_ = subscription.drain(index);
                if (state.pending_.refresh_at_ &&
                    (!state.refresh_at_ || *state.pending_.refresh_at_ < *state.refresh_at_)) {
                    state.refresh_at_ = state.pending_.refresh_at_;
                }
            }
        }
    } catch (const std::exception& error) {
        if (service::common::sse_client_disconnected(error)) {
            co_return;
        }
        throw;
    }
}

template <typename fetch_type>
ruvia::task<void> stream_snapshot(ruvia::context& c, hub::subscription subscription, fetch_type fetch,
    std::string_view snapshot_event = "snapshot") {
    std::vector<snapshot_part> parts;
    parts.push_back({snapshot_event, std::move(fetch)});
    co_await stream_snapshots(c, std::move(subscription), std::move(parts));
}

}  // namespace service::live_resource
