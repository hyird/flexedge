#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>

#include "service/features/background/worker_pool.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dns/runtime.h"

namespace service::website_dispatch {

namespace detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};

struct WebsiteMarker final {
    std::string id;
    std::string tenantId;
    std::string resourceId;
    std::string operation;
    std::int64_t version{};
    std::int64_t failures{};
};

inline std::string boundedError(std::string_view value) {
    return std::string(value.substr(0, std::min<std::size_t>(value.size(), 1000)));
}

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
        if (!locked.empty()) {
            (void)co_await service::sync_runtime::upsertMarker(transaction, tenantId, "website",
                                                               websiteId, "apply", revision);
        }
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void> recoverStaleMarkers(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(context.db(), "website");
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
        "marker.id, marker.tenant_id, marker.resource_id, marker.operation, marker.version, "
        "marker.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return WebsiteMarker{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .resourceId = std::string(row[2].value().value_or("")),
        .operation = std::string(row[3].value().value_or("apply")),
        .version = row[4].as<std::int64_t>().value_or(1),
        .failures = row[5].as<std::int64_t>().value_or(0),
    };
}

inline ruvia::Task<void> execute(service::background::WorkerContext& context,
                                 const WebsiteMarker& marker) {
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = marker.tenantId, .markerId = marker.id, .version = marker.version},
        .owner = context.leaseOwner(),
    };
    co_await service::website_dns::probeWebsite(context, marker.resourceId, lease, marker.version);
    co_return;
}

inline ruvia::Task<void> fail(service::background::WorkerContext& context,
                              const WebsiteMarker& marker, std::string_view error) {
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = marker.tenantId, .markerId = marker.id, .version = marker.version},
        .owner = context.leaseOwner(),
    };
    auto transaction = co_await context.db().beginTransaction();
    const auto transitioned =
        co_await service::sync_runtime::failRunning(transaction, lease, boundedError(error));
    co_await transaction.commit();
    if (transitioned) {
        service::logging::error("Website sync marker " + marker.id +
                                " failed: " + boundedError(error));
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    auto nextLeaseRecovery = std::chrono::steady_clock::now();
    auto nextReconciliation = std::chrono::steady_clock::now();
    while (!context.stopToken().stopRequested()) {
        bool processed = false;
        try {
            if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
                co_await recoverStaleMarkers(context);
                nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
            }
            if (std::chrono::steady_clock::now() >= nextReconciliation) {
                co_await reconcileMarkers(context);
                nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
            }
            if (const auto marker = co_await claim(context)) {
                processed = true;
                std::string markerError;
                try {
                    co_await execute(context, *marker);
                } catch (const std::exception& error) {
                    markerError = boundedError(error.what());
                } catch (...) {
                    markerError = "网站同步发生未知错误";
                }
                if (!markerError.empty()) {
                    co_await fail(context, *marker, markerError);
                }
            }
        } catch (const std::exception& error) {
            service::logging::error("Website sync worker failure: " + boundedError(error.what()));
        } catch (...) {
            service::logging::error("Website sync worker failure: unknown error");
        }
        if (!processed &&
            co_await ruvia::sleepFor(context.worker(), kIdlePollInterval, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::website_dispatch
