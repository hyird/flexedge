#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "service/features/background/task_context.h"
#include "service/features/certificate/task.h"
#include "service/features/live_resource/notification_outbox.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::task<std::optional<certificate_task>>
claim(service::background::task_context& context) {
    auto transaction = co_await context.db().begin_transaction();
    const auto rows = co_await transaction.query(
        "WITH candidate AS (SELECT marker.id FROM sys_sync_task marker INNER JOIN sys_certificate "
        "certificate ON certificate.tenant_id = marker.tenant_id AND certificate.id = "
        "marker.resource_id WHERE marker.resource_type = 'certificate' AND NOT marker.is_done AND "
        "marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "certificate.deleted_at IS NULL AND certificate.issuance_revision = marker.version ORDER "
        "BY "
        "marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP LOCKED LIMIT "
        "1) "
        "UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 "
        "seconds', "
        "updated_at = NOW() FROM candidate WHERE marker.id = candidate.id RETURNING marker.id, "
        "marker.tenant_id, marker.resource_id, marker.version, marker.count_fails, "
        "marker.operation, " +
            service::sync_runtime::claimed_owner_columns("marker"),
        context.lease_owner());
    if (rows.empty()) {
        co_await transaction.commit();
        co_return std::nullopt;
    }
    const auto claimed_at = std::chrono::steady_clock::now();
    const auto& row = rows.front();
    auto owner = service::sync_runtime::claimed_owner(row, 6);
    service::live_resource::notifications::resource_notification notification(row[1].value().value_or(""));
    notification.changed(service::live_resource::resource::certificates, row[2].value().value_or(""));
    if (!owner.operation_id_.empty()) {
        notification.changed(service::live_resource::resource::operations, owner.operation_id_);
    }
    co_await service::live_resource::commit_and_notify(transaction, context, notification);
    co_return certificate_task{
        .id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .certificate_id_ = std::string(row[2].value().value_or("")),
        .version_ = row[3].as<std::int64_t>().value_or(1),
        .failures_ = row[4].as<std::int64_t>().value_or(0),
        .operation_ = std::string(row[5].value().value_or("")),
        .claimed_at_ = claimed_at,
        .owner_ = std::move(owner),
    };
}

}  // namespace service::certificate_issuance::worker_detail
