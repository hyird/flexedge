#pragma once

#include <ruvia/core/task.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/dns_sync/task.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::dns_sync::detail {

// Recover a missing task, including an interrupted deletion. Existing failed
// or running tasks retain their retry policy and ownership.
template <typename context_type>
inline ruvia::task<void> reconcile_tasks(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    ruvia::db_query candidates(context.pool());
    candidates.select({candidates.column("tenant_id", "zone"), candidates.column("id", "zone"),
                          candidates.column("desired_revision", "zone"), candidates.sql("zone.deleted_at IS NOT NULL")})
        .from("sys_dns_zone", "zone")
        .where(candidates.sql(
            "(zone.deleted_at IS NULL OR zone.synced_revision < zone.desired_revision) "
            "AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE marker.resource_type = 'dns_zone' "
            "AND marker.tenant_id = zone.tenant_id AND marker.resource_id = zone.id "
            "AND marker.version = zone.desired_revision)"))
        .order_by(candidates.column("sort", "zone"))
        .limit(32)
        .lock({.mode_ = ruvia::db_row_lock::update, .skip_locked_ = true});
    const auto missing = co_await transaction.query(candidates);
    service::background::queued_work work;
    std::vector<service::live_resource::notifications::resource_notification> notifications;
    for (const auto& row : missing) {
        const auto tenant_id = row[0].value().value_or("");
        const auto zone_id = row[1].value().value_or("");
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
            service::sync_runtime::marker_resource_type::dns_zone, zone_id,
            row[3].template as<bool>().value_or(false) ? service::sync_runtime::marker_operation::remove
                                                       : service::sync_runtime::marker_operation::sync,
            row[2].template as<std::int64_t>().value_or(1), work);
        auto notification = std::ranges::find_if(notifications,
            [&](const auto& item) { return item.tenant_id_ == tenant_id; });
        if (notification == notifications.end()) {
            notifications.emplace_back(tenant_id).changed(service::live_resource::resource::tasks, zone_id);
        } else {
            notification->changed(service::live_resource::resource::tasks, zone_id);
        }
    }
    std::vector<std::string> notification_ids;
    notification_ids.reserve(notifications.size());
    for (const auto& notification : notifications) {
        notification_ids.push_back(co_await service::live_resource::enqueue_notification(transaction, notification));
    }
    co_await transaction.commit();
    for (std::size_t index = 0; index < notifications.size(); ++index) {
        co_await service::live_resource::wake_committed_notification(
            context, notification_ids[index], notifications[index]);
    }
    service::background::work_available::wake_own_claims(context, work.claims());
}

// A settled task lends its existing lease to a periodic inspection. It stays
// settled in the UI unless the inspection finds a change or an actual failure.
template <typename context_type>
inline ruvia::task<std::optional<dns_task>> claim_due_check(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "WITH due AS (SELECT task.id FROM sys_dns_zone zone JOIN sys_sync_task task ON "
        "task.tenant_id = zone.tenant_id AND task.resource_type = 'dns_zone' AND task.resource_id = zone.id "
        "JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id "
        "WHERE zone.deleted_at IS NULL AND provider.deleted_at IS NULL AND provider.status = 'verified' "
        "AND zone.next_check_at <= NOW() AND task.version = zone.desired_revision AND task.is_done "
        "AND task.lease_until IS NULL AND task.wait_task_id IS NULL "
        "ORDER BY zone.next_check_at, zone.sort LIMIT 1 FOR UPDATE OF zone, task SKIP LOCKED) "
        "UPDATE sys_sync_task task SET lease_owner = $1, lease_until = NOW() + $2 * INTERVAL '1 second' "
        "FROM due WHERE task.id = due.id RETURNING task.id, task.tenant_id, task.resource_id, "
        "task.version, task.count_fails",
        context.lease_owner(), std::int64_t{service::sync_runtime::lease_duration.count()});
    if (rows.empty()) {
        co_await transaction.commit();
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    dns_task result{.id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .resource_id_ = std::string(row[2].value().value_or("")),
        .operation_ = service::sync_runtime::marker_operation::sync,
        .version_ = row[3].template as<std::int64_t>().value_or(1),
        .failures_ = 0,
        .claimed_at_ = std::chrono::steady_clock::now(),
        .inspection_ = true};
    co_await transaction.commit();
    co_return result;
}

}  // namespace service::dns_sync::detail
