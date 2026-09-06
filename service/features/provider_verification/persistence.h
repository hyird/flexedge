#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/logging/logger.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/provider_verification/queue.h"
#include "service/features/provider_verification/task.h"
#include "service/features/provider_verification/verification.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::provider_verification::detail {

inline ruvia::Task<void> reconcile(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "SELECT marker.tenant_id, marker.id, marker.resource_id, provider.verification_generation "
        "FROM sys_sync_task marker INNER JOIN sys_provider provider ON provider.tenant_id = "
        "marker.tenant_id AND provider.id = marker.resource_id WHERE marker.resource_type = "
        "'provider' AND provider.deleted_at IS NULL AND marker.version <> "
        "provider.verification_generation LIMIT 64");
    for (const auto& row : rows) {
        auto transaction = co_await context.db().beginTransaction();
        const auto providerId = std::string(row[2].value().value_or(""));
        co_await service::provider_verification::markCurrent(
            transaction, row[0].value().value_or(""), providerId);
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<std::optional<std::string>>
loadCurrentRuntime(service::background::WorkerContext& context, const VerificationTask& task) {
    const auto lease = service::sync_runtime::makeRunningLease(
        task.tenantId, task.id, task.generation, context.leaseOwner());
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }
    const auto rows = co_await context.db().query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1",
        task.providerId, task.tenantId, task.kind, task.providerRevision, task.generation);
    if (!rows.empty()) {
        co_return std::string(rows.front()[0].value().value_or("{}"));
    }
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    co_await transaction.commit();
    co_return std::nullopt;
}

inline ruvia::Task<void> completeVerification(service::background::WorkerContext& context,
                                              const VerificationTask& task,
                                              VerificationResult result) {
    const bool dns = task.kind == "dns";
    const auto lease = service::sync_runtime::makeRunningLease(
        task.tenantId, task.id, task.generation, context.leaseOwner());
    auto transaction = co_await context.db().beginTransaction();
    const auto rows = co_await transaction.query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = $3 "
        "AND revision = $4 AND verification_generation = $5 AND deleted_at IS NULL LIMIT 1 FOR "
        "UPDATE",
        task.providerId, task.tenantId, task.kind, task.providerRevision, task.generation);
    if (rows.empty()) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
        throw std::runtime_error("供应商检测标记 lease 已失效");
    }

    std::string runtimeJson;
    if (dns) {
        if (!result.dnsRuntime) {
            throw VerificationError("DNS 服务商检测结果缺失", true);
        }
        runtimeJson = std::move(*result.dnsRuntime);
    } else {
        const auto runtime = service::certificate_issuance::parseCertificateProviderRuntime(
            rows.front()[0].value().value_or("{}"), {.resource = context.resource()});
        if (!runtime) {
            throw VerificationError("证书供应商 runtime 损坏", true);
        }
        runtimeJson = service::certificate_issuance::serializeCertificateProviderRuntime(
            runtime, result.eab, context.resource());
    }
    const auto updated = co_await transaction.execute(
        "UPDATE sys_provider SET runtime = $1::jsonb, status = 'verified', last_verified_at = "
        "NOW(), last_error = NULL, updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND "
        "revision = $4 AND verification_generation = $5 AND deleted_at IS NULL",
        std::string_view(runtimeJson), task.providerId, task.tenantId, task.providerRevision,
        task.generation);
    service::sync_runtime::RunningResultTransition resultTransition;
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    } else {
        if (dns) {
            co_await service::dns_sync::markProviderZonesDirty(transaction, task.tenantId,
                                                               task.providerId);
        }
        resultTransition =
            co_await service::sync_runtime::completeRunningAndRecordEvent(transaction, lease);
        if (!resultTransition.markerTransitioned) {
            throw std::runtime_error("供应商检测标记 lease 已失效");
        }
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    co_return;
}

inline ruvia::Task<void> failVerification(service::background::WorkerContext& context,
                                          const VerificationTask& task, std::string_view error,
                                          bool permanent) {
    const auto message = service::sync_runtime::boundedError(error);
    const auto lease = service::sync_runtime::makeRunningLease(
        task.tenantId, task.id, task.generation, context.leaseOwner());
    auto transaction = co_await context.db().beginTransaction();
    const auto resultTransition =
        co_await service::sync_runtime::failRunningAndRecordEvent(transaction, lease, message);
    if (resultTransition.markerTransitioned) {
        (void)co_await transaction.execute(
            "UPDATE sys_provider SET status = CASE WHEN $1::BOOLEAN THEN 'invalid' ELSE status "
            "END, last_error = $2, updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND "
            "verification_generation = $5 AND deleted_at IS NULL",
            permanent, std::string_view(message), task.providerId, task.tenantId, task.generation);
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    if (resultTransition.markerTransitioned) {
        service::logging::error("Provider verification marker " + task.id + " failed: " + message);
    }
    co_return;
}

} // namespace service::provider_verification::detail
