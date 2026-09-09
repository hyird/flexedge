#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>

#include <ruvia/core/Task.h>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/persistence.h"
#include "service/features/website_dispatch/task.h"
#include "service/features/website_dns/runtime.h"

namespace service::website_dispatch {

namespace detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{8};

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(
        context.db(), service::sync_runtime::MarkerResourceType::website);
    co_return;
}

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const WebsiteMarker& marker) {
    const auto lease = service::sync_runtime::makeRunningLease(
        marker.tenantId, marker.id, marker.version, context.leaseOwner(),
        service::sync_runtime::MarkerResourceType::website, marker.resourceId);
    co_await service::website_dns::probeWebsite(context, marker.resourceId, lease, marker.version);
    co_return;
}

inline ruvia::Task<void> processMarker(service::background::WorkerContext& context,
                                       const WebsiteMarker& marker) {
    std::string markerError;
    try {
        co_await execute(context, marker);
    } catch (const std::exception& error) {
        markerError = service::sync_runtime::boundedError(error.what());
    } catch (...) {
        markerError = "网站同步发生未知错误";
    }
    if (!context.stopToken().stopRequested() && !markerError.empty()) {
        co_await failWebsiteMarker(context, marker, markerError);
    }
    co_return;
}

inline ruvia::Task<void> processMarkers(service::background::WorkerContext& context,
                                        std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        if (context.stopToken().stopRequested()) {
            break;
        }
        const auto marker = co_await claim(context);
        if (context.stopToken().stopRequested() || !marker) {
            break;
        }
        co_await processMarker(context, *marker);
    }
    co_return;
}

inline ruvia::Task<void> runMaintenance(service::background::WorkerContext& context,
                                        std::chrono::steady_clock::time_point& nextLeaseRecovery,
                                        std::chrono::steady_clock::time_point& nextReconciliation) {
    if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
        co_await recoverStaleMarkers(context);
        nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
    }
    if (context.stopToken().stopRequested()) {
        co_return;
    }
    if (std::chrono::steady_clock::now() >= nextReconciliation) {
        co_await reconcileMarkers(context);
        nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    co_await service::background::runMarkerWorkerLoop(
        context, kIdlePollInterval, "Website sync worker failure: ", "网站同步发生未知错误",
        runMaintenance, processMarkers, service::sync_runtime::boundedError);
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::website_dispatch
