#pragma once

#include "service/features/node_dispatch/notifications.h"

#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/task.h"
#include "service/features/certificate_material/model.h"
#include "service/features/logging/logger.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::Task<bool>
markIssuanceStarted(service::background::WorkerContext& context, const CertificateTask& task,
                    const service::sync_runtime::RunningMarkerLease& lease) {
    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET status = CASE WHEN issued_revision = 0 THEN 'issuing' ELSE "
        "'renewing' END, last_error = NULL, updated_at = NOW() WHERE id = $1 AND tenant_id = "
        "$2 AND issuance_revision = $3 AND deleted_at IS NULL",
        task.certificateId, task.tenantId, task.version);
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return false;
    }
    if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
        throw std::runtime_error("证书同步标记 lease 已失效");
    }
    co_await transaction.commit();
    co_return true;
}

inline ruvia::Task<void>
persistIssuedCertificate(service::background::WorkerContext& context, const CertificateTask& task,
                         const service::sync_runtime::RunningMarkerLease& lease,
                         const IssuedCertificate& issued) {
    const auto nowRows =
        co_await context.db().query("SELECT TO_CHAR(NOW(), 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')");
    service::certificate_material::CertificateMaterialOutput material(
        {.resource = context.resource()});
    material.set<"certificateChainPem">(issued.certificateChainPem);
    material.set<"privateKeyEnvelope">(service::utils::sealSecret(issued.privateKeyPem.view()));
    material.set<"notBefore">(issued.notBefore);
    material.set<"serialNumber">(issued.serialNumber);
    material.set<"fingerprintSha256">(issued.fingerprintSha256);
    material.set<"lastIssuedAt">(nowRows.front()[0].value().value_or(""));
    const auto materialJson = ruvia::toJson(material, {.resource = context.resource()});

    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_certificate SET material = $1::jsonb, expires_at = $2::timestamptz, "
        "issued_revision = $3, status = 'valid', last_error = NULL, updated_at = NOW() WHERE id = "
        "$4 AND tenant_id = $5 AND issuance_revision = $3 AND deleted_at IS NULL",
        std::string_view(materialJson), std::string_view(issued.expiresAt), task.version,
        task.certificateId, task.tenantId);
    service::sync_runtime::RunningResultTransition resultTransition;
    if (updated.affectedRows() != 0) {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
        co_await service::node_dispatch::enqueueCertificateConsumers(transaction, task.tenantId,
                                                                     task.certificateId);
        resultTransition =
            co_await service::sync_runtime::completeRunningAndRecordEvent(transaction, lease);
        if (!resultTransition.markerTransitioned) {
            throw std::runtime_error("证书同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    service::node_dispatch::notifications::published(task.tenantId);
    co_return;
}

inline ruvia::Task<void> failCertificateTask(service::background::WorkerContext& context,
                                             const CertificateTask& task, std::string_view error,
                                             bool permanent) {
    const auto message = service::sync_runtime::boundedError(error);
    const auto lease = service::sync_runtime::makeRunningLease(task.tenantId, task.id, task.version,
                                                               context.leaseOwner());
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenantId, task.certificateId);
    const auto resultTransition =
        co_await service::sync_runtime::failRunningAndRecordEvent(transaction, lease, message);
    if (resultTransition.markerTransitioned) {
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = CASE WHEN expires_at > NOW() THEN CASE WHEN $3 "
            "THEN 'valid' ELSE 'renewing' END WHEN $3 THEN 'failed' ELSE 'pending' END, "
            "last_error = $4, updated_at = NOW() WHERE id = $1 AND tenant_id = $5 AND "
            "issuance_revision = $2 AND deleted_at IS NULL RETURNING issued_revision > 0 AND "
            "expires_at <= NOW()",
            task.certificateId, task.version, permanent, std::string_view(message), task.tenantId);
        if (!updated.empty() && updated.front()[0].as<bool>().value_or(false)) {
            co_await service::node_dispatch::enqueueCertificateConsumers(transaction, task.tenantId,
                                                                         task.certificateId);
        }
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    service::node_dispatch::notifications::published(task.tenantId);
    if (!resultTransition.markerTransitioned) {
        co_return;
    }
    service::logging::error("Certificate sync marker " + task.id + " failed: " + message);
    co_return;
}

} // namespace service::certificate_issuance::worker_detail
