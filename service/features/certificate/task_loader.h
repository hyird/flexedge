#pragma once

#include "service/features/live_resource/fanout.h"

#include <cstdint>
#include <optional>
#include <string>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/certificate/task.h"

namespace service::certificate_issuance::worker_detail {

inline ruvia::Task<std::optional<CertificateTask>>
claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
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
        "marker.tenant_id, marker.resource_id, marker.version, marker.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    service::live_resource::hub().publish(row[1].value().value_or(""),
                                         service::live_resource::Resource::tasks,
                                         row[0].value().value_or(""));
    service::live_resource::hub().publish(row[1].value().value_or(""),
                                         service::live_resource::Resource::certificates,
                                         row[2].value().value_or(""));
    co_return CertificateTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .certificateId = std::string(row[2].value().value_or("")),
        .version = row[3].as<std::int64_t>().value_or(1),
        .failures = row[4].as<std::int64_t>().value_or(0),
    };
}

} // namespace service::certificate_issuance::worker_detail
