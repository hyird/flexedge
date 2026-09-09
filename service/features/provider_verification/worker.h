#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

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

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{8};

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(
        context.db(), service::sync_runtime::MarkerResourceType::provider);
    co_return;
}

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const VerificationTask& task) {
    const auto currentRuntime = co_await loadCurrentRuntime(context, task);
    if (!currentRuntime) {
        co_return;
    }
    VerificationResult result;
    if (task.kind == "dns") {
        result = co_await verifyDns(context, task);
    } else if (task.kind == "certificate") {
        result = co_await verifyCertificate(context, task, *currentRuntime);
    } else {
        throw VerificationError("供应商类型无效", true);
    }
    co_await completeVerification(context, task, std::move(result));
}

inline ruvia::Task<void> processMarker(service::background::WorkerContext& context,
                                       const VerificationTask& task) {
    std::exception_ptr exception;
    try {
        co_await execute(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (!context.stopToken().stopRequested() && exception) {
        const auto failure = classifyTaskFailure(exception);
        co_await failVerification(context, task, failure.message, failure.permanent);
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
        co_await reconcile(context);
        nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    co_await service::background::runMarkerWorkerLoop(
        context, kIdlePollInterval, "Provider verification worker failure: ", "未知供应商检测错误",
        runMaintenance, processMarkers, service::sync_runtime::boundedError);
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::provider_verification
