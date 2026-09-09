#pragma once

#include "service/features/live_resource/fanout.h"

#include <cstdint>
#include <optional>
#include <string>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/task.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

inline ruvia::Task<std::optional<DnsTask>> claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT task.id FROM sys_sync_task task INNER JOIN sys_dns_zone zone ON "
        "zone.tenant_id = task.tenant_id AND zone.id = task.resource_id WHERE "
        "task.resource_type = 'dns_zone' AND NOT task.is_done AND task.next_attempt_at <= NOW() "
        "AND task.lease_until IS NULL AND task.version = zone.desired_revision AND "
        "(zone.deleted_at IS NULL OR task.operation = 'delete') ORDER BY task.next_attempt_at ASC, "
        "task.updated_at ASC FOR UPDATE OF task SKIP LOCKED LIMIT 1) UPDATE sys_sync_task task "
        "SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 seconds', updated_at = NOW() "
        "FROM candidate WHERE task.id = candidate.id RETURNING task.id, task.tenant_id, "
        "task.resource_id, task.operation, task.version, task.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    service::live_resource::hub().publish(row[1].value().value_or(""),
                                         service::live_resource::Resource::tasks,
                                         row[0].value().value_or(""));
    service::live_resource::hub().publish(row[1].value().value_or(""),
                                         service::live_resource::Resource::dnsZones,
                                         row[2].value().value_or(""));
    co_return DnsTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .resourceId = std::string(row[2].value().value_or("")),
        .operation = service::sync_runtime::requireMarkerOperation(row[3].value().value_or("")),
        .version = row[4].as<std::int64_t>().value_or(1),
        .failures = row[5].as<std::int64_t>().value_or(0),
    };
}

} // namespace service::dns_sync::detail
