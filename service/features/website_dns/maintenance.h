#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>

#include "service/features/sync_runtime/worker.h"

namespace service::website_dns::detail {

// All undeleted websites are checked, including domains managed outside FlexEdge.
// The existing settled task owns both its next inspection time and the inspection lease.
template <typename context_type>
inline ruvia::task<std::optional<service::sync_runtime::claimed_marker>> claim_due_check(context_type& context) {
    auto transaction = co_await context.db().begin_transaction();
    auto rows = co_await transaction.query(
        "WITH due AS (SELECT task.id FROM sys_sync_task task JOIN sys_website website ON "
        "website.tenant_id = task.tenant_id AND website.id = task.resource_id "
        "WHERE task.resource_type = 'website' AND website.deleted_at IS NULL "
        "AND task.version = website.revision AND task.is_done AND task.next_attempt_at <= NOW() "
        "AND task.lease_until IS NULL AND task.wait_task_id IS NULL "
        "ORDER BY task.next_attempt_at, task.id LIMIT 1 FOR UPDATE OF website, task SKIP LOCKED) "
        "UPDATE sys_sync_task task SET lease_owner = $1, lease_until = NOW() + $2 * INTERVAL '1 second' "
        "FROM due WHERE task.id = due.id RETURNING task.id, task.tenant_id, task.resource_id, task.version",
        context.lease_owner(), std::int64_t{service::sync_runtime::lease_duration.count()});
    if (rows.empty()) {
        co_await transaction.commit();
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    service::sync_runtime::claimed_marker result{.id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .resource_id_ = std::string(row[2].value().value_or("")),
        .operation_ = service::sync_runtime::marker_operation::apply,
        .version_ = row[3].template as<std::int64_t>().value_or(1),
        .claimed_at_ = std::chrono::steady_clock::now(),
        .row_ = std::move(rows)};
    co_await transaction.commit();
    co_return result;
}

}  // namespace service::website_dns::detail
