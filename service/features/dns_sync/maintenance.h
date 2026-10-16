#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <string>

#include "service/features/background/task_context.h"
#include "service/features/dns_sync/queue.h"

namespace service::dns_sync::detail {

// Prunes obsolete cluster-managed records (a changed zone becomes the system
// task 「清理过期 DNS 记录」) and re-queues zones whose current revision lost
// its marker; the subtask of that revision keeps the work.
inline ruvia::task<void> reconcile_tasks(service::background::task_context& context) {
    const auto zones = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_dns_zone WHERE deleted_at IS NULL ORDER BY sort ASC LIMIT "
        "128");
    for (const auto& zone : zones) {
        const auto tenant_id = std::string(zone[0].value().value_or(""));
        const auto zone_id = std::string(zone[1].value().value_or(""));
        auto origin =
            service::operation::work_origin::system(service::operation::action::system_dns_cleanup, "dns_zone", zone_id);
        auto transaction = co_await context.db().begin_transaction();
        (void)co_await service::dns_sync::prune_cluster_managed_records(transaction, tenant_id, zone_id, origin);
        co_await service::sync_runtime::commit_origin(transaction, context, tenant_id, origin);
    }
    const auto missing = co_await context.db().query(
        "SELECT zone.tenant_id, zone.id, zone.desired_revision FROM sys_dns_zone zone WHERE "
        "zone.deleted_at IS NULL AND zone.synced_revision < zone.desired_revision AND NOT EXISTS "
        "(SELECT 1 FROM sys_sync_task task WHERE task.resource_type = 'dns_zone' AND "
        "task.tenant_id = zone.tenant_id AND task.resource_id = zone.id AND "
        "task.version = zone.desired_revision AND NOT task.is_done) ORDER BY zone.sort ASC LIMIT "
        "32");
    for (const auto& row : missing) {
        const auto tenant_id = std::string(row[0].value().value_or(""));
        auto origin = service::operation::work_origin::repair();
        auto transaction = co_await context.db().begin_transaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT 1 FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
            "desired_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await service::dns_sync::enqueue_zone_revision(
                transaction, tenant_id, std::string(row[1].value().value_or("")), revision, origin);
        }
        co_await service::sync_runtime::commit_origin(transaction, context, tenant_id, origin);
    }
    co_return;
}

}  // namespace service::dns_sync::detail
