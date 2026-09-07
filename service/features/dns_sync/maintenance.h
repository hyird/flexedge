#pragma once

#include <cstdint>
#include <string>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/queue.h"

namespace service::dns_sync::detail {

inline ruvia::Task<void> reconcileTasks(service::background::WorkerContext& context) {
    const auto zones = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_dns_zone WHERE deleted_at IS NULL ORDER BY sort ASC LIMIT "
        "128");
    for (const auto& zone : zones) {
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await service::dns_sync::pruneClusterManagedRecords(
            transaction, std::string(zone[0].value().value_or("")),
            std::string(zone[1].value().value_or("")));
        co_await transaction.commit();
    }
    const auto missing = co_await context.db().query(
        "SELECT zone.tenant_id, zone.id, zone.desired_revision FROM sys_dns_zone zone WHERE "
        "zone.deleted_at IS NULL AND zone.synced_revision < zone.desired_revision AND NOT EXISTS "
        "(SELECT 1 FROM sys_sync_task task WHERE task.resource_type = 'dns_zone' AND "
        "task.tenant_id = zone.tenant_id AND task.resource_id = zone.id AND "
        "task.version = zone.desired_revision AND NOT task.is_done) ORDER BY zone.sort ASC LIMIT "
        "32");
    for (const auto& row : missing) {
        auto transaction = co_await context.db().beginTransaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT 1 FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
            "desired_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await service::dns_sync::enqueueZoneRevision(
                transaction, std::string(row[0].value().value_or("")),
                std::string(row[1].value().value_or("")), revision);
        }
        co_await transaction.commit();
    }
    co_return;
}

} // namespace service::dns_sync::detail
