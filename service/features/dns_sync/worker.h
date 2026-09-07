#pragma once

#include <chrono>
#include <cstddef>
#include <exception>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

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

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{32};

inline ruvia::Task<void> recoverStaleTasks(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(
        context.db(), service::sync_runtime::MarkerResourceType::dnsZone);
    co_return;
}

inline ruvia::Task<void> processTask(service::background::WorkerContext& context,
                                     const DnsTask& task) {
    std::exception_ptr exception;
    try {
        (void)co_await syncZone(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception) {
        const auto failure = classifyTaskFailure(exception);
        co_await failDnsTask(context, task, failure.message, failure.permanent);
    }
    co_return;
}

inline ruvia::Task<void> processTasks(service::background::WorkerContext& context,
                                      std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        const auto task = co_await claim(context);
        if (!task) {
            break;
        }
        co_await processTask(context, *task);
    }
    co_return;
}

inline ruvia::Task<void> runMaintenance(service::background::WorkerContext& context,
                                        std::chrono::steady_clock::time_point& nextLeaseRecovery,
                                        std::chrono::steady_clock::time_point& nextReconciliation) {
    if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
        co_await recoverStaleTasks(context);
        nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
    }
    if (std::chrono::steady_clock::now() >= nextReconciliation) {
        co_await reconcileTasks(context);
        nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    co_await service::background::runMarkerWorkerLoop(
        context, kIdlePollInterval, "DNS sync worker failure: ", "未知 DNS 同步错误",
        runMaintenance, processTasks, service::sync_runtime::boundedError);
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::dns_sync
