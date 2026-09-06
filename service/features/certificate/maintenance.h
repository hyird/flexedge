#pragma once

#include <cstdint>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/model.h"
#include "service/features/certificate/queue.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::Task<void> expireCertificates(service::background::WorkerContext& context) {
    const auto expired = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND "
        "issued_revision > 0 AND expires_at <= NOW() AND status <> 'expired' ORDER BY sort ASC");
    for (const auto& row : expired) {
        auto transaction = co_await context.db().beginTransaction();
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = 'expired', updated_at = NOW() WHERE tenant_id "
            "= $1 AND id = $2 AND deleted_at IS NULL AND issued_revision > 0 AND expires_at <= "
            "NOW() AND status <> 'expired' RETURNING id",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (!updated.empty()) {
            co_await service::node_dispatch::enqueueCertificateConsumers(
                transaction, row[0].value().value_or(""), updated.front()[0].value().value_or(""));
        }
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void>
reconcileMissingIssuanceMarkers(service::background::WorkerContext& context) {
    const auto missing = co_await context.db().query(
        "SELECT tenant_id, id, issuance_revision FROM sys_certificate certificate WHERE "
        "certificate.deleted_at IS NULL AND certificate.issued_revision < "
        "certificate.issuance_revision "
        "AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE marker.resource_type = "
        "'certificate' AND marker.tenant_id = certificate.tenant_id AND marker.resource_id = "
        "certificate.id AND marker.version = certificate.issuance_revision)");
    for (const auto& row : missing) {
        auto transaction = co_await context.db().beginTransaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT issued_revision FROM sys_certificate WHERE tenant_id = $1 AND id = $2 AND "
            "issuance_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await enqueueCertificateRevision(
                transaction, row[0].value().value_or(""), row[1].value().value_or(""), revision,
                locked.front()[0].as<std::int64_t>().value_or(0) == 0
                    ? service::sync_runtime::MarkerOperation::issue
                    : service::sync_runtime::MarkerOperation::renew);
        }
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void> scheduleAutoRenewals(service::background::WorkerContext& context) {
    const auto candidates = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND expires_at "
        "<= NOW() + INTERVAL '30 days' AND status IN ('valid', 'expired') ORDER BY sort ASC");
    for (const auto& row : candidates) {
        auto transaction = co_await context.db().beginTransaction();
        const auto locked = co_await transaction.query(
            "SELECT issuance_revision, config::text FROM sys_certificate WHERE tenant_id = $1 "
            "AND id = $2 AND deleted_at IS NULL AND expires_at <= NOW() + INTERVAL '30 days' AND "
            "status IN ('valid', 'expired') FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (locked.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto config = parseConfigStored(locked.front()[1].value().value_or("{}"),
                                              {.resource = context.resource()});
        if (!config || !config->autoRenew) {
            co_await transaction.commit();
            continue;
        }
        const auto active = co_await transaction.query(
            "SELECT id FROM sys_sync_task WHERE resource_type = 'certificate' AND tenant_id = $1 "
            "AND resource_id = $2 AND NOT is_done LIMIT 1",
            row[0].value().value_or(""), row[1].value().value_or(""));
        if (!active.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto issuanceRevision = locked.front()[0].as<std::int64_t>().value_or(0) + 1;
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = 'renewing', last_error = "
            "NULL, updated_at = NOW() WHERE tenant_id = $2 AND id = $3",
            issuanceRevision, row[0].value().value_or(""), row[1].value().value_or(""));
        co_await enqueueCertificateRevision(transaction, row[0].value().value_or(""),
                                            row[1].value().value_or(""), issuanceRevision,
                                            service::sync_runtime::MarkerOperation::renew);
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void>
reconcileCertificateMaintenance(service::background::WorkerContext& context) {
    co_await expireCertificates(context);
    co_await reconcileMissingIssuanceMarkers(context);
    co_await scheduleAutoRenewals(context);
    co_return;
}

} // namespace service::certificate_issuance::worker_detail
