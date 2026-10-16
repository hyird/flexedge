#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "service/features/background/task_context.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/task.h"

namespace service::website_dispatch::detail {

inline ruvia::task<void> reconcile_markers(service::background::task_context& context) {
    auto db = context.db();
    const auto missing = co_await db.query(
        "SELECT website.tenant_id, website.id, website.revision FROM sys_website website WHERE "
        "website.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE "
        "marker.tenant_id = website.tenant_id AND marker.resource_type = 'website' AND "
        "marker.resource_id = website.id AND marker.version = website.revision) ORDER BY "
        "website.updated_at ASC LIMIT 64");
    for (const auto& row : missing) {
        auto origin = service::operation::work_origin::repair();
        auto transaction = co_await db.begin_transaction();
        const auto tenant_id = std::string(row[0].value().value_or(""));
        const auto website_id = std::string(row[1].value().value_or(""));
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT revision FROM sys_website WHERE tenant_id = $1 AND id = $2 AND revision = $3 "
            "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            tenant_id, website_id, revision);
        if (!locked.empty()) {
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
                service::sync_runtime::marker_resource_type::website, website_id,
                service::sync_runtime::marker_operation::apply, revision, origin);
        }
        co_await service::sync_runtime::commit_origin(transaction, context, tenant_id, origin);
    }
    co_return;
}

inline ruvia::task<std::optional<website_marker>>
claim(service::background::task_context& context) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "WITH candidate AS (SELECT marker.id FROM sys_sync_task marker INNER JOIN sys_website "
        "website ON website.tenant_id = marker.tenant_id AND website.id = marker.resource_id "
        "WHERE marker.resource_type = 'website' AND NOT marker.is_done AND "
        "marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "marker.version = website.revision AND website.deleted_at IS NULL ORDER BY "
        "marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP LOCKED LIMIT "
        "1) "
        "UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 "
        "seconds', updated_at = NOW() FROM candidate WHERE marker.id = candidate.id RETURNING "
        "marker.id, marker.tenant_id, marker.resource_id, marker.version, marker.count_fails, " +
            service::sync_runtime::claimed_owner_columns("marker"),
        context.lease_owner());
    if (rows.empty()) {
        co_await transaction.commit();
        co_return std::nullopt;
    }
    const auto claimed_at = std::chrono::steady_clock::now();
    const auto& row = rows.front();
    const auto owner = service::sync_runtime::claimed_owner(row, 5);
    service::live_resource::notifications::resource_notification notification(row[1].value().value_or(""));
    if (!owner.operation_id_.empty()) {
        notification.changed(service::live_resource::resource::operations, owner.operation_id_);
    }
    co_await service::live_resource::commit_and_notify(transaction, context, notification);
    co_return website_marker{
        .id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .resource_id_ = std::string(row[2].value().value_or("")),
        .version_ = row[3].as<std::int64_t>().value_or(1),
        .failures_ = row[4].as<std::int64_t>().value_or(0),
        .claimed_at_ = claimed_at,
    };
}

inline ruvia::task<void> fail_website_marker(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const website_marker& marker, std::string_view error,
    std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        marker.tenant_id_, marker.id_, marker.version_, context.lease_owner(), marker.claimed_at_,
        service::sync_runtime::marker_resource_type::website, marker.resource_id_);
    auto transaction = co_await context.db().begin_transaction();
    const auto result_transition = co_await service::sync_runtime::fail_running_and_record_event(
        transaction, lease, message, detail);
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace);
    if (result_transition.marker_transitioned_) {
        service::sync_runtime::log_attempt_failure("Website sync marker", marker.id_, message);
    }
    co_return;
}

}  // namespace service::website_dispatch::detail
