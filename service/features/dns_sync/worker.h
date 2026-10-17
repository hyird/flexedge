#pragma once

#include <ruvia/core/task.h>

#include <exception>
#include <string>

#include "service/features/background/task_context.h"
#include "service/features/dns_sync/maintenance.h"
#include "service/features/dns_sync/sync_engine.h"
#include "service/features/dns_sync/task.h"
#include "service/features/outbound_http/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/worker.h"

namespace service::dns_sync {

namespace detail {

inline ruvia::task<void> process_task(service::background::task_context& context,
    const dns_task& task) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web",
        task.claimed_at_);
    zone_sync_attempt attempt;
    std::exception_ptr exception;
    try {
        (void)co_await sync_zone(context, trace, task, attempt);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.get_stop_token().stop_requested() && exception) {
        const auto failure = service::outbound_http::classify_attempt_failure(exception, "同步任务发生未知错误");
        const bool terminal = failure.permanent_ || task.failures_ >= 2;
        auto step = service::sync_runtime::record_attempt_failure_step(
            trace, failure.message_, terminal, "本次同步失败，等待下次检查或手动重试");
        co_await fail_dns_task(context, trace, task, failure.message_, terminal,
            attempt.to_json(context.pool()));
        step.complete();
    }
    co_return;
}

inline ruvia::task<void> process_marker(service::background::task_context& context,
    const service::sync_runtime::claimed_marker& marker) {
    co_await process_task(context,
        dns_task{.id_ = marker.id_,
            .tenant_id_ = marker.tenant_id_,
            .resource_id_ = marker.resource_id_,
            .operation_ = marker.operation_,
            .version_ = marker.version_,
            .failures_ = marker.failures_,
            .claimed_at_ = marker.claimed_at_});
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::task_context& context) {
    auto maintain = [next_repair = std::chrono::steady_clock::time_point{}](service::background::task_context& owned) mutable -> ruvia::task<void> {
        if (std::chrono::steady_clock::now() >= next_repair) {
            co_await detail::reconcile_tasks(owned);
            next_repair = std::chrono::steady_clock::now() + service::sync_runtime::reconciliation_interval;
        }
        const auto limit = service::sync_runtime::describe(service::sync_runtime::marker_resource_type::dns_zone).claims_per_round_;
        for (std::size_t count = 0; count < limit && !owned.get_stop_token().stop_requested(); ++count) {
            const auto inspection = co_await detail::claim_due_check(owned);
            if (!inspection) {
                break;
            }
            co_await detail::process_task(owned, *inspection);
        }
    };
    co_await service::sync_runtime::run_marker_worker(context, service::sync_runtime::marker_resource_type::dns_zone,
        maintain, detail::process_marker, {}, std::chrono::seconds(15));
}

}  // namespace service::dns_sync
