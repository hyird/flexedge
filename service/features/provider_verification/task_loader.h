#pragma once

#include <ruvia/core/task.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

#include "service/features/background/worker_pool.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/provider_verification/task.h"
#include "service/features/sync_runtime/state.h"

namespace service::provider_verification::detail {

inline ruvia::task<std::optional<verification_task>>
claim(service::background::worker_context& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT marker.id, provider.kind, provider.provider, provider.name, "
        "provider.account_id, provider.revision, provider.config::text AS config, "
        "provider.runtime::text AS runtime FROM sys_sync_task marker INNER JOIN sys_provider "
        "provider ON provider.tenant_id = marker.tenant_id AND provider.id = marker.resource_id "
        "WHERE marker.resource_type = 'provider' AND marker.operation = 'verify' AND NOT "
        "marker.is_done AND marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "provider.deleted_at IS NULL AND provider.verification_generation = marker.version "
        "ORDER BY marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP "
        "LOCKED LIMIT 1) UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + "
        "INTERVAL '60 seconds', updated_at = NOW() FROM candidate WHERE marker.id = candidate.id "
        "RETURNING marker.id, marker.tenant_id, marker.resource_id, marker.version, "
        "marker.count_fails, candidate.kind, candidate.provider, candidate.name, "
        "candidate.account_id, "
        "candidate.revision, candidate.config, candidate.runtime, " +
            service::sync_runtime::claimed_owner_columns("marker"),
        context.lease_owner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto claimed_at = std::chrono::steady_clock::now();
    const auto& row = rows.front();
    auto owner = service::sync_runtime::claimed_owner(row, 12);
    service::live_resource::notifications::resource_notification notification(row[1].value().value_or(""));
    if (!owner.operation_id_.empty()) {
        notification.changed(service::live_resource::resource::operations, owner.operation_id_);
    }
    co_await service::live_resource::notify(context, notification);
    co_return verification_task{
        .id_ = std::string(row[0].value().value_or("")),
        .tenant_id_ = std::string(row[1].value().value_or("")),
        .provider_id_ = std::string(row[2].value().value_or("")),
        .kind_ = std::string(row[5].value().value_or("")),
        .provider_ = std::string(row[6].value().value_or("")),
        .name_ = std::string(row[7].value().value_or("")),
        .account_id_ = std::string(row[8].value().value_or("")),
        .provider_revision_ = row[9].as<std::int64_t>().value_or(0),
        .generation_ = row[3].as<std::int64_t>().value_or(0),
        .failures_ = row[4].as<std::int64_t>().value_or(0),
        .config_json_ = std::string(row[10].value().value_or("{}")),
        .runtime_json_ = std::string(row[11].value().value_or("{}")),
        .claimed_at_ = claimed_at,
        .owner_ = std::move(owner),
    };
}

}  // namespace service::provider_verification::detail
