#pragma once

#include <ruvia/core/task.h>

#include <cstdint>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/config_mapper.h"
#include "service/features/certificate/queue.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/sync_runtime/state.h"

namespace service::certificate_issuance::worker_detail {

// Expired material leaves the consumer clusters: the system task 「证书过期」.
inline ruvia::task<void> expire_certificates(service::background::worker_context& context) {
    const auto expired = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND "
        "issued_revision > 0 AND expires_at <= NOW() AND status <> 'expired' ORDER BY sort ASC");
    for (const auto& row : expired) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto certificate_id = std::string(row[1].value().value_or(""));
        auto origin = service::operation::work_origin::system(
            service::operation::action::system_certificate_expired, "certificate", certificate_id);
        auto transaction = co_await context.db().begin_transaction();
        const auto updated = co_await transaction.query(
            "UPDATE sys_certificate SET status = 'expired', updated_at = NOW() WHERE tenant_id "
            "= $1 AND id = $2 AND deleted_at IS NULL AND issued_revision > 0 AND expires_at <= "
            "NOW() AND status <> 'expired' RETURNING id",
            tenant_id, certificate_id);
        if (!updated.empty()) {
            co_await service::node_dispatch::enqueue_certificate_consumers(transaction, tenant_id, certificate_id,
                origin);
        }
        co_await transaction.commit();
        if (!updated.empty()) {
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.node_config().changed(service::live_resource::resource::certificates, certificate_id);
            origin.add_changes(notification);
            co_await service::live_resource::notify(context, notification);
        }
    }
    co_return;
}

// A lost marker of the current issuance revision is re-created; the subtask
// of that revision keeps the work.
inline ruvia::task<void>
reconcile_missing_issuance_markers(service::background::worker_context& context) {
    const auto missing = co_await context.db().query(
        "SELECT tenant_id, id, issuance_revision FROM sys_certificate certificate WHERE "
        "certificate.deleted_at IS NULL AND certificate.issued_revision < "
        "certificate.issuance_revision "
        "AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE marker.resource_type = "
        "'certificate' AND marker.tenant_id = certificate.tenant_id AND marker.resource_id = "
        "certificate.id AND marker.version = certificate.issuance_revision)");
    for (const auto& row : missing) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        auto origin = service::operation::work_origin::repair();
        auto transaction = co_await context.db().begin_transaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT issued_revision FROM sys_certificate WHERE tenant_id = $1 AND id = $2 AND "
            "issuance_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            tenant_id, row[1].value().value_or(""), revision);
        const bool changed = !locked.empty();
        if (changed) {
            co_await enqueue_certificate_revision(transaction, tenant_id, row[1].value().value_or(""), revision,
                locked.front()[0].as<std::int64_t>().value_or(0) == 0 ? service::sync_runtime::marker_operation::issue
                                                                      : service::sync_runtime::marker_operation::renew,
                origin);
        }
        co_await transaction.commit();
        if (changed) {
            service::live_resource::notifications::resource_notification notification(tenant_id);
            notification.changed(service::live_resource::resource::certificates, row[1].value().value_or(""));
            origin.add_changes(notification);
            co_await service::live_resource::notify(context, notification);
        }
    }
    co_return;
}

// Certificates expiring within 30 days with automatic renewal: each renewal
// is the system task 「自动续期」.
inline ruvia::task<void> schedule_auto_renewals(service::background::worker_context& context) {
    const auto candidates = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_certificate WHERE deleted_at IS NULL AND expires_at "
        "<= NOW() + INTERVAL '30 days' AND status IN ('valid', 'expired') ORDER BY sort ASC");
    for (const auto& row : candidates) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto certificate_id = std::string(row[1].value().value_or(""));
        auto transaction = co_await context.db().begin_transaction();
        const auto locked = co_await transaction.query(
            "SELECT issuance_revision, config::text FROM sys_certificate WHERE tenant_id = $1 "
            "AND id = $2 AND deleted_at IS NULL AND expires_at <= NOW() + INTERVAL '30 days' AND "
            "status IN ('valid', 'expired') FOR UPDATE",
            tenant_id, certificate_id);
        if (locked.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto config = parse_config_stored(locked.front()[1].value().value_or("{}"),
            {.resource_ = context.pool()});
        if (!config || !config->auto_renew_) {
            co_await transaction.commit();
            continue;
        }
        const auto active = co_await transaction.query(
            "SELECT id FROM sys_sync_task WHERE resource_type = 'certificate' AND tenant_id = $1 "
            "AND resource_id = $2 AND NOT is_done LIMIT 1",
            tenant_id, certificate_id);
        if (!active.empty()) {
            co_await transaction.commit();
            continue;
        }
        const auto issuance_revision = locked.front()[0].as<std::int64_t>().value_or(0) + 1;
        (void)co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = 'renewing', last_error = "
            "NULL, updated_at = NOW() WHERE tenant_id = $2 AND id = $3",
            issuance_revision, tenant_id, certificate_id);
        auto origin = service::operation::work_origin::system(
            service::operation::action::system_certificate_auto_renew, "certificate", certificate_id);
        co_await enqueue_certificate_revision(transaction, tenant_id, certificate_id, issuance_revision,
            service::sync_runtime::marker_operation::renew, origin);
        co_await transaction.commit();
        service::live_resource::notifications::resource_notification notification(tenant_id);
        notification.changed(service::live_resource::resource::certificates, certificate_id);
        origin.add_changes(notification);
        co_await service::live_resource::notify(context, notification);
    }
    co_return;
}

inline ruvia::task<void>
reconcile_certificate_maintenance(service::background::worker_context& context) {
    co_await expire_certificates(context);
    co_await reconcile_missing_issuance_markers(context);
    co_await schedule_auto_renewals(context);
    co_return;
}

}  // namespace service::certificate_issuance::worker_detail
