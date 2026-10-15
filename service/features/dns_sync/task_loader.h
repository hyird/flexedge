#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "service/features/background/worker_pool.h"
#include "service/features/dns_sync/task.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

inline ruvia::task<std::optional<dns_task>> claim(service::background::worker_context& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT task.id FROM sys_sync_task task INNER JOIN sys_dns_zone zone ON "
        "zone.tenant_id = task.tenant_id AND zone.id = task.resource_id WHERE "
        "task.resource_type = 'dns_zone' AND NOT task.is_done AND task.next_attempt_at <= NOW() "
        "AND task.lease_until IS NULL AND task.version = zone.desired_revision AND "
        "(zone.deleted_at IS NULL OR task.operation = 'delete') ORDER BY task.next_attempt_at ASC, "
        "task.updated_at ASC FOR UPDATE OF task SKIP LOCKED LIMIT 1) UPDATE sys_sync_task task "
        "SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 seconds', updated_at = NOW() "
        "FROM candidate WHERE task.id = candidate.id RETURNING task.id, task.tenant_id, "
        "task.resource_id, task.operation, task.version, task.count_fails, " +
            service::sync_runtime::claimed_owner_columns("task"),
        context.lease_owner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto claimed_at = std::chrono::steady_clock::now();
    const auto& row = rows.front();
    auto owner = service::sync_runtime::claimed_owner(row, 6);
    service::live_resource::notifications::resource_notification notification(row[1].value().value_or(""));
    notification.changed(service::live_resource::resource::dns_zones, row[2].value().value_or(""));
    if (!owner.operation_id_.empty()) {
        notification.changed(service::live_resource::resource::operations, owner.operation_id_);
    }
    co_await service::live_resource::notify(context, notification);
    co_return dns_task{
        .id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .resource_id_ = std::string(row[2].value().value_or("")),
        .operation_ = service::sync_runtime::require_marker_operation(row[3].value().value_or("")),
        .version_ = row[4].as<std::int64_t>().value_or(1),
        .failures_ = row[5].as<std::int64_t>().value_or(0),
        .claimed_at_ = claimed_at,
        .owner_ = std::move(owner),
    };
}

}  // namespace service::dns_sync::detail
