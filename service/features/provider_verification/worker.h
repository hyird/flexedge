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
    const verification_task& task) {
    const auto current_runtime = co_await load_current_runtime(context, task);
    if (!current_runtime) {
        co_return;
    }
    verification_result result;
    if (task.kind_ == "dns") {
        result = co_await verify_dns(context, task);
    } else if (task.kind_ == "certificate") {
        result = co_await verify_certificate(context, task, *current_runtime);
    } else {
        throw verification_error("供应商类型无效", true);
    }
    co_await complete_verification(context, task, std::move(result));
}

inline ruvia::task<void> process_marker(service::background::worker_context& context,
    const verification_task& task) {
    std::exception_ptr exception;
    try {
        co_await execute(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.get_stop_token().stop_requested() && exception) {
        const auto failure = classify_task_failure(exception);
        co_await fail_verification(context, task, failure.message_, failure.permanent_);
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
        run_maintenance, process_markers, service::sync_runtime::bounded_error);
    co_return;
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::worker_context& context) {
    co_await detail::run(context);
}

}  // namespace service::provider_verification
