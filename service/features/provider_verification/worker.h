#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <utility>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/provider_verification/persistence.h"
#include "service/features/provider_verification/task.h"
#include "service/features/provider_verification/task_loader.h"
#include "service/features/provider_verification/verification.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::provider_verification {

namespace detail {

inline constexpr std::chrono::seconds idle_poll_interval{2};
inline constexpr std::chrono::seconds lease_recovery_interval{15};
inline constexpr std::chrono::minutes reconciliation_interval{15};
inline constexpr std::size_t max_jobs_per_tick{8};

inline ruvia::task<void> recover_stale_markers(service::background::worker_context& context) {
    auto db = context.db();
    co_await service::sync_runtime::recover_stale_running(
        db, service::sync_runtime::marker_resource_type::provider);
    co_return;
}

inline ruvia::task<void> execute(service::background::worker_context& context,
    service::attempt_trace::recorder& trace, const verification_task& task, provider_attempt& attempt) {
    auto load_step = trace.begin_step(service::attempt_trace::step_code::load_provider, task.name_);
    const auto current_runtime = co_await load_current_runtime(context, task);
    if (!current_runtime) {
        load_step.skip("供应商配置已更新，本次让位于新的检测");
        co_return;
    }
    load_step.complete();
    verification_result result;
    if (task.kind_ == "dns") {
        result = co_await verify_dns(context, trace, task, attempt);
    } else if (task.kind_ == "certificate") {
        result = co_await verify_certificate(context, trace, task, *current_runtime, attempt);
    } else {
        throw verification_error("供应商类型无效", true);
    }
    auto save_step = trace.begin_step(service::attempt_trace::step_code::save_result);
    co_await complete_verification(context, trace, task, std::move(result), attempt.to_json(context.pool()));
    save_step.complete();
}

inline ruvia::task<void> process_marker(service::background::worker_context& context,
    const verification_task& task) {
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::provider, "provider-verification",
        task.claimed_at_);
    provider_attempt attempt(task.provider_, task.kind_);
    std::exception_ptr exception;
    try {
        co_await execute(context, trace, task, attempt);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.get_stop_token().stop_requested() && exception) {
        const auto failure = classify_task_failure(exception);
        trace.mark_failure(failure.message_);
        auto step = trace.begin_step(service::attempt_trace::step_code::record_failure);
        trace.log(failure.permanent_ ? service::attempt_trace::log_level::error
                                     : service::attempt_trace::log_level::warning,
            failure.permanent_ ? "永久失败，供应商标记为无效，不再自动重试" : "将在 15 秒后自动重试");
        co_await fail_verification(context, trace, task, failure.message_, failure.permanent_,
            attempt.to_json(context.pool()));
        step.complete();
    }
    co_return;
}

inline ruvia::task<void> process_markers(service::background::worker_context& context,
    std::size_t& processed) {
    for (; processed < max_jobs_per_tick; ++processed) {
        if (context.get_stop_token().stop_requested()) {
            break;
        }
        const auto marker = co_await claim(context);
        if (context.get_stop_token().stop_requested() || !marker) {
            break;
        }
        co_await process_marker(context, *marker);
    }
    co_return;
}

inline ruvia::task<void> run_maintenance(service::background::worker_context& context,
    std::chrono::steady_clock::time_point& next_lease_recovery,
    std::chrono::steady_clock::time_point& next_reconciliation) {
    if (std::chrono::steady_clock::now() >= next_lease_recovery) {
        co_await recover_stale_markers(context);
        next_lease_recovery = std::chrono::steady_clock::now() + lease_recovery_interval;
    }
    if (context.get_stop_token().stop_requested()) {
        co_return;
    }
    if (std::chrono::steady_clock::now() >= next_reconciliation) {
        co_await reconcile(context);
        next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;
    }
    co_return;
}

inline ruvia::task<void> run(service::background::worker_context& context) {
    co_await service::background::run_marker_worker_loop(
        context, idle_poll_interval, "Provider verification worker failure: ", "未知供应商检测错误",
        run_maintenance, process_markers, service::sync_runtime::worker_error);
    co_return;
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::worker_context& context) {
    co_await detail::run(context);
}

}  // namespace service::provider_verification
