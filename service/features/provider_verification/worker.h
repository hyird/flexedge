#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/provider_verification/persistence.h"
#include "service/features/provider_verification/task.h"
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

inline ruvia::Task<std::optional<VerificationTask>>
claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT marker.id, provider.kind, provider.provider, provider.name, "
        "provider.account_id, provider.revision, provider.config::text AS config, "
        "provider.runtime::text AS runtime FROM sys_sync_task marker INNER JOIN sys_provider "
        "provider ON provider.tenant_id = marker.tenant_id AND provider.id = marker.resource_id "
        "WHERE marker.resource_type = 'provider' AND marker.operation = 'verify' AND NOT "
        "marker.is_done AND marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "provider.deleted_at IS NULL AND provider.verification_generation = marker.version "
        "ORDER BY marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP "
        "LOCKED LIMIT 1) UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + "
        "INTERVAL '60 seconds', updated_at = NOW() FROM candidate WHERE marker.id = candidate.id "
        "RETURNING marker.id, marker.tenant_id, marker.resource_id, marker.version, "
        "marker.count_fails, candidate.kind, candidate.provider, candidate.name, "
        "candidate.account_id, "
        "candidate.revision, candidate.config, candidate.runtime",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return VerificationTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .providerId = std::string(row[2].value().value_or("")),
        .kind = std::string(row[5].value().value_or("")),
        .provider = std::string(row[6].value().value_or("")),
        .name = std::string(row[7].value().value_or("")),
        .accountId = std::string(row[8].value().value_or("")),
        .providerRevision = row[9].as<std::int64_t>().value_or(0),
        .generation = row[3].as<std::int64_t>().value_or(0),
        .failures = row[4].as<std::int64_t>().value_or(0),
        .configJson = std::string(row[10].value().value_or("{}")),
        .runtimeJson = std::string(row[11].value().value_or("{}")),
    };
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
    if (exception) {
        const auto failure = classifyTaskFailure(exception);
        co_await failVerification(context, task, failure.message, failure.permanent);
    }
    co_return;
}

inline ruvia::Task<void> processMarkers(service::background::WorkerContext& context,
                                        std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        const auto marker = co_await claim(context);
        if (!marker) {
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
