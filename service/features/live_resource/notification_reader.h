#pragma once

#include <ruvia/core/operation_options.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>

#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/node_runtime.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/logging/logger.h"
#include "service/features/node_dispatch/fanout.h"

namespace service::live_resource {

inline constexpr auto notification_retry_delay = std::chrono::seconds(1);

[[nodiscard]] inline std::int64_t unix_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Every subscription of the calling worker reads one recovery snapshot and
// every node control connection it holds re-reads its desired state.
template <typename context_type>
void recover_subscriptions(context_type& context, std::optional<std::string_view> tenant = std::nullopt) {
    get_hub(context).recover(tenant);
    auto& dispatch = service::node_dispatch::fanout::get_hub(context);
    if (tenant) {
        dispatch.publish(*tenant);
    } else {
        dispatch.publish_all();
    }
}

// Delivers one stream entry to the calling worker's own subscriptions.
// `recovered_at`: when this reader last recovered from losing the stream; an
// owed recovery of a producer from before that instant is already covered.
template <typename context_type>
void deliver_notification(context_type& context, const notifications::resource_notification& value,
    std::optional<std::int64_t> recovered_at) {
    if (value.missed_until_ && (!recovered_at || *recovered_at < *value.missed_until_)) {
        if (value.tenant_id_ == notifications::all_tenants) {
            recover_subscriptions(context);
        } else {
            recover_subscriptions(context, std::string_view(value.tenant_id_));
        }
    }
    if (value.tenant_id_ == notifications::all_tenants) {
        return;
    }
    auto& hub_value = get_hub(context);
    const auto& tenant = value.tenant_id_;
    for (const auto& change : value.changes_) {
        hub_value.publish(tenant, change.resource_, change.id_);
    }
    for (const auto& attempt : value.task_attempts_) {
        hub_value.publish_task_attempt(tenant, attempt.task_id_, attempt.notice_);
    }
    for (const auto& task_id : value.task_history_resets_) {
        hub_value.reset_task_history(tenant, task_id);
    }
    if (!value.node_id_.empty()) {
        get_node_deadlines(context).touch(tenant, value.node_id_);
    }
    if (!value.node_runtime_.empty()) {
        hub_value.publish_runtime(tenant, value.node_id_, value.node_runtime_);
    }
    for (const auto& origin : value.origin_runtime_) {
        hub_value.publish_origins(tenant, origin.website_id_, value.node_id_, origin.payload_);
    }
    if (value.node_config_) {
        service::node_dispatch::fanout::get_hub(context).publish(tenant);
    }
}

// The read loop of run_notification_reader. It starts at the newest entry
// (fresh subscriptions read initial snapshots) and, after losing the stream
// (Redis restart, timeout, network), recovers every subscription of this
// worker once the stream is readable again: entries appended meanwhile may
// have been lost or never sent.
template <typename context_type>
ruvia::task<void> read_notifications(context_type& context) {
    std::string cursor;
    bool initialized{};
    bool interrupted{};
    std::optional<std::int64_t> recovered_at;
    while (!context.get_stop_token().stop_requested()) {
        bool retry{};
        try {
            const auto redis = context.redis();
            if (!initialized) {
                cursor = co_await notifications::current_cursor(redis);
                initialized = true;
            }
            auto batch = co_await notifications::read(redis, cursor);
            if (interrupted) {
                recover_subscriptions(context);
                recovered_at = unix_milliseconds();
                interrupted = false;
                service::logging::info("Resource notifications resumed; subscriptions read recovery snapshots");
            }
            if (batch.cursor_) {
                cursor = *batch.cursor_;
            }
            for (const auto& item : batch.entries_) {
                if (item.notification_) {
                    deliver_notification(context, *item.notification_, recovered_at);
                }
            }
        } catch (const std::exception& error) {
            if (!context.get_stop_token().stop_requested()) {
                service::logging::error("Resource notification reader failure: " + std::string(error.what()));
                retry = true;
            }
        }
        if (retry) {
            interrupted = true;
            if (co_await ruvia::sleep_for(context.worker(), notification_retry_delay, context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
        }
    }
}

// One copy per Web Worker, on that worker's own Redis connection: reads the
// resource notifications for this worker's subscriptions and, beside it,
// re-sends the recoveries this worker's sender owes after a failed send.
template <typename context_type>
ruvia::task<void> run_notification_reader(context_type& context) {
    ruvia::task_scope retries(context.worker());
    retries.spawn(context.template worker_state<notifications::sender>().retry_owed(
        context, ruvia::combine_stop_tokens(context.get_stop_token(), retries.get_stop_token())));
    std::exception_ptr failure;
    try {
        co_await read_notifications(context);
    } catch (...) {
        failure = std::current_exception();
    }
    retries.request_stop();
    co_await retries.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace service::live_resource
