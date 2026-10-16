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
    if (exception) {
        const auto failure = service::outbound_http::classify_attempt_failure(exception, "同步任务发生未知错误");
        trace.mark_failure(failure.message_);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_failure);
        trace.log(failure.permanent_ ? service::attempt_trace::log_level::error
                                     : service::attempt_trace::log_level::warning,
            failure.permanent_ ? "永久失败，不再自动重试" : "将在 15 秒后自动重试");
        co_await fail_dns_task(context, trace, task, failure.message_, failure.permanent_,
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
    co_await service::sync_runtime::run_marker_worker(context, service::sync_runtime::marker_resource_type::dns_zone,
        detail::reconcile_tasks, detail::process_marker);
}

}  // namespace service::dns_sync
