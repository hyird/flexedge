#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/provider_verification/task.h"

namespace service::provider_verification::detail {

inline ruvia::Task<std::optional<VerificationTask>>
claim(service::background::WorkerContext& context) {
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
        "candidate.revision, candidate.config, candidate.runtime",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return VerificationTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .providerId = std::string(row[2].value().value_or("")),
        .kind = std::string(row[5].value().value_or("")),
        .provider = std::string(row[6].value().value_or("")),
        .name = std::string(row[7].value().value_or("")),
        .accountId = std::string(row[8].value().value_or("")),
        .providerRevision = row[9].as<std::int64_t>().value_or(0),
        .generation = row[3].as<std::int64_t>().value_or(0),
        .failures = row[4].as<std::int64_t>().value_or(0),
        .configJson = std::string(row[10].value().value_or("{}")),
        .runtimeJson = std::string(row[11].value().value_or("{}")),
    };
}

} // namespace service::provider_verification::detail
