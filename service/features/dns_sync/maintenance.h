#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <string>

#include "service/features/background/queued_work.h"
#include "service/features/background/task_context.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::dns_sync::detail {

// Prunes obsolete cluster-managed records and re-queues zones whose current
// revision lost its marker.
inline ruvia::task<void> reconcile_tasks(service::background::task_context& context) {
    const auto zones = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_dns_zone WHERE deleted_at IS NULL ORDER BY sort ASC LIMIT "
        "128");
    for (const auto& zone : zones) {
        const auto tenant_id = std::string(zone[0].value().value_or(""));
        const auto zone_id = std::string(zone[1].value().value_or(""));
        service::background::queued_work work;
        auto transaction = co_await context.db().begin_transaction();
        (void)co_await service::dns_sync::prune_cluster_managed_records(transaction, tenant_id, zone_id, work);
        co_await service::sync_runtime::commit_work(transaction, context, tenant_id, work);
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
        service::background::queued_work work;
        auto transaction = co_await context.db().begin_transaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT 1 FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
            "desired_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await service::dns_sync::enqueue_zone_revision(
                transaction, tenant_id, std::string(row[1].value().value_or("")), revision, work);
        }
        co_await service::sync_runtime::commit_work(transaction, context, tenant_id, work);
    }
    co_return;
}

}  // namespace service::dns_sync::detail
