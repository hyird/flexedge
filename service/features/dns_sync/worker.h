#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <exception>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/failure.h"
#include "service/features/dns_sync/maintenance.h"
#include "service/features/dns_sync/sync_engine.h"
#include "service/features/dns_sync/task.h"
#include "service/features/dns_sync/task_loader.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync {

namespace detail {

inline constexpr std::chrono::seconds idle_poll_interval{2};
inline constexpr std::chrono::seconds lease_recovery_interval{15};
inline constexpr std::chrono::minutes reconciliation_interval{15};
inline constexpr std::size_t max_jobs_per_tick{32};

inline ruvia::task<void> recover_stale_tasks(service::background::worker_context& context) {
    auto db = context.db();
    co_await service::sync_runtime::recover_stale_running(
        db, service::sync_runtime::marker_resource_type::dns_zone);
    co_return;
}

inline ruvia::task<void> process_task(service::background::worker_context& context,
    const dns_task& task) {
    std::exception_ptr exception;
    try {
        (void)co_await sync_zone(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception) {
        const auto failure = classify_task_failure(exception);
        co_await fail_dns_task(context, task, failure.message_, failure.permanent_);
    }
    co_return;
}

inline ruvia::task<void> process_tasks(service::background::worker_context& context,
    std::size_t& processed) {
    for (; processed < max_jobs_per_tick; ++processed) {
        const auto task = co_await claim(context);
        if (!task) {
            break;
        }
        co_await process_task(context, *task);
    }
    co_return;
}

inline ruvia::task<void> run_maintenance(service::background::worker_context& context,
    std::chrono::steady_clock::time_point& next_lease_recovery,
    std::chrono::steady_clock::time_point& next_reconciliation) {
    if (std::chrono::steady_clock::now() >= next_lease_recovery) {
        co_await recover_stale_tasks(context);
        next_lease_recovery = std::chrono::steady_clock::now() + lease_recovery_interval;
    }
    if (std::chrono::steady_clock::now() >= next_reconciliation) {
        co_await reconcile_tasks(context);
        next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;
    }
    co_return;
}

inline ruvia::task<void> run(service::background::worker_context& context) {
    co_await service::background::run_marker_worker_loop(
        context, idle_poll_interval, "DNS sync worker failure: ", "未知 DNS 同步错误",
        run_maintenance, process_tasks, service::sync_runtime::bounded_error);
    co_return;
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::worker_context& context) {
    co_await detail::run(context);
}

}  // namespace service::dns_sync
