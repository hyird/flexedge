#pragma once

#include "service/features/live_resource/fanout.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>

#include "service/features/background/worker_pool.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dispatch/task.h"

namespace service::website_dispatch::detail {

inline ruvia::Task<void> reconcileMarkers(service::background::WorkerContext& context) {
    const auto missing = co_await context.db().query(
        "SELECT website.tenant_id, website.id, website.revision FROM sys_website website WHERE "
        "website.deleted_at IS NULL AND NOT EXISTS (SELECT 1 FROM sys_sync_task marker WHERE "
        "marker.tenant_id = website.tenant_id AND marker.resource_type = 'website' AND "
        "marker.resource_id = website.id AND marker.version = website.revision) ORDER BY "
        "website.updated_at ASC LIMIT 64");
    for (const auto& row : missing) {
        auto transaction = co_await context.db().beginTransaction();
        const auto tenantId = std::string(row[0].value().value_or(""));
        const auto websiteId = std::string(row[1].value().value_or(""));
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT revision FROM sys_website WHERE tenant_id = $1 AND id = $2 AND revision = $3 "
            "AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            tenantId, websiteId, revision);
        std::string markerId;
        if (!locked.empty()) {
            markerId = co_await service::sync_runtime::upsertMarker(
                transaction, tenantId, service::sync_runtime::MarkerResourceType::website,
                websiteId, service::sync_runtime::MarkerOperation::apply, revision);
        }
        co_await transaction.commit();
        if (!markerId.empty())
            service::live_resource::hub().publish(tenantId, service::live_resource::Resource::tasks,
                                                  markerId);
    }
    co_return;
}

inline ruvia::Task<std::optional<WebsiteMarker>>
claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT marker.id FROM sys_sync_task marker INNER JOIN sys_website "
        "website ON website.tenant_id = marker.tenant_id AND website.id = marker.resource_id "
        "WHERE marker.resource_type = 'website' AND NOT marker.is_done AND "
        "marker.next_attempt_at <= NOW() AND marker.lease_until IS NULL AND "
        "marker.version = website.revision AND website.deleted_at IS NULL ORDER BY "
        "marker.next_attempt_at ASC, marker.updated_at ASC FOR UPDATE OF marker SKIP LOCKED LIMIT "
        "1) "
        "UPDATE sys_sync_task marker SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 "
        "seconds', updated_at = NOW() FROM candidate WHERE marker.id = candidate.id RETURNING "
        "marker.id, marker.tenant_id, marker.resource_id, marker.version, marker.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    service::live_resource::hub().publish(row[1].value().value_or(""),
                                         service::live_resource::Resource::tasks,
                                         row[0].value().value_or(""));
    co_return WebsiteMarker{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .resourceId = std::string(row[2].value().value_or("")),
        .version = row[3].as<std::int64_t>().value_or(1),
        .failures = row[4].as<std::int64_t>().value_or(0),
    };
}

inline ruvia::Task<void> failWebsiteMarker(service::background::WorkerContext& context,
                                           const WebsiteMarker& marker, std::string_view error) {
    const auto message = service::sync_runtime::boundedError(error);
    const auto lease = service::sync_runtime::makeRunningLease(
        marker.tenantId, marker.id, marker.version, context.leaseOwner(),
        service::sync_runtime::MarkerResourceType::website, marker.resourceId);
    auto transaction = co_await context.db().beginTransaction();
    const auto resultTransition =
        co_await service::sync_runtime::failRunningAndRecordEvent(transaction, lease, message);
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    if (resultTransition.markerTransitioned) {
        service::logging::error("Website sync marker " + marker.id + " failed: " + message);
    }
    co_return;
}

} // namespace service::website_dispatch::detail
