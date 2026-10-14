#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/persistence.h"
#include "service/features/website_dispatch/task.h"
#include "service/features/website_dns/runtime.h"

namespace service::website_dispatch {

namespace detail {

inline constexpr std::chrono::seconds idle_poll_interval{2};
inline constexpr std::chrono::seconds lease_recovery_interval{15};
inline constexpr std::chrono::minutes reconciliation_interval{15};
inline constexpr std::size_t max_jobs_per_tick{8};

inline ruvia::task<void> recover_stale_markers(service::background::worker_context& context) {
    auto db = context.db();
    co_await service::sync_runtime::recover_stale_running(
        db, service::sync_runtime::marker_resource_type::website);
    co_return;
}

inline ruvia::task<void> execute(service::background::worker_context& context,
    const website_marker& marker) {
    const auto lease = service::sync_runtime::make_running_lease(
        marker.tenant_id_, marker.id_, marker.version_, context.lease_owner(),
        service::sync_runtime::marker_resource_type::website, marker.resource_id_);
    co_await service::website_dns::probe_website(context, marker.resource_id_, lease, marker.version_);
    co_return;
}

inline ruvia::task<void> process_marker(service::background::worker_context& context,
    const website_marker& marker) {
    std::string marker_error;
    try {
        co_await execute(context, marker);
    } catch (const std::exception& error) {
        marker_error = service::sync_runtime::bounded_error(error.what());
    } catch (...) {
        marker_error = "网站同步发生未知错误";
    }
    if (!context.get_stop_token().stop_requested() && !marker_error.empty()) {
        co_await fail_website_marker(context, marker, marker_error);
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
        co_await reconcile_markers(context);
        next_reconciliation = std::chrono::steady_clock::now() + reconciliation_interval;
    }
    co_return;
}

inline ruvia::task<void> run(service::background::worker_context& context) {
    co_await service::background::run_marker_worker_loop(
        context, idle_poll_interval, "Website sync worker failure: ", "网站同步发生未知错误",
        run_maintenance, process_markers, service::sync_runtime::bounded_error);
    co_return;
}

}  // namespace detail

inline ruvia::task<void> run_worker(service::background::worker_context& context) {
    co_await detail::run(context);
}

}  // namespace service::website_dispatch
