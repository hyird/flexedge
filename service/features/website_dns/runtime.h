#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelJson.h>

#include "service/features/background/worker_pool.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_dns/mapper.h"
#include "service/features/website_dns/probe.h"
#include "service/features/sync_runtime/state.h"

namespace service::website_dns {

inline ruvia::Task<void>
completeStaleWebsiteTask(service::background::WorkerContext& context,
                         const service::sync_runtime::RunningMarkerLease& marker,
                         bool revisionChanged) {
    auto completion = co_await context.db().beginTransaction();
    if (revisionChanged) {
        (void)co_await service::sync_runtime::releaseRunning(completion, marker);
    } else {
        (void)co_await service::sync_runtime::completeRunning(completion, marker);
    }
    co_await completion.commit();
    co_return;
}

inline ruvia::Task<WebsiteRuntimeData>
probeWebsiteDomains(service::background::WorkerContext& context,
                    const service::website_config::WebsiteConfigData& config,
                    std::string_view expectedTarget, std::string_view checkedAt,
                    const service::sync_runtime::RunningMarkerLease& marker) {
    WebsiteRuntimeData output;
    auto& domainStates = output.domainStates;
    for (const auto& domain : config.domains) {
        if (!co_await service::sync_runtime::renewRunningLease(context.db(), marker)) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
        auto& state = domainStates.emplace_back();
        state.id = domain.id;
        state.lastVerifiedAt = std::string(checkedAt);
        try {
            const auto result =
                co_await probeCname(context, domain.hostname, expectedTarget, domain.id);
            state.resolutionStatus = result.matched ? "verified" : "invalid";
            if (!result.matched) {
                const auto observed =
                    result.observedTarget.empty() ? "未查询到 CNAME" : result.observedTarget;
                state.lastError = "期望 " + std::string(expectedTarget) + "，实际 " + observed;
            }
        } catch (const std::exception& error) {
            state.resolutionStatus = "unverified";
            state.lastError = error.what();
        }
    }
    co_return output;
}

inline ruvia::Task<void> probeWebsite(service::background::WorkerContext& context,
                                      const std::string& websiteId,
                                      const service::sync_runtime::RunningMarkerLease& marker,
                                      std::int64_t expectedRevision) {
    const auto rows = co_await context.db().query(
        "SELECT website.config::text, website.runtime::text, cluster.hostname_prefix || '.' || "
        "zone.domain, website.revision, website.deleted_at IS NOT NULL, TO_CHAR(NOW(), "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_website website INNER JOIN sys_cluster "
        "cluster ON cluster.tenant_id = website.tenant_id AND cluster.id = "
        "website.cluster_id INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
        "cluster.tenant_id AND zone.id = cluster.dns_zone_id WHERE website.id = $1 AND "
        "website.tenant_id = $2 LIMIT 1",
        websiteId, marker.marker.tenantId);
    const bool revisionChanged =
        !rows.empty() && rows.front()[3].as<std::int64_t>().value_or(0) != expectedRevision;
    const bool resourceDeleted = rows.empty() || rows.front()[4].as<bool>().value_or(false);
    if (resourceDeleted) {
        auto completion = co_await context.db().beginTransaction();
        (void)co_await service::sync_runtime::completeRunning(completion, marker);
        co_await completion.commit();
        co_return;
    }
    if (rows.empty() || revisionChanged) {
        co_await completeStaleWebsiteTask(context, marker, revisionChanged);
        co_return;
    }

    const auto config = service::website_config::parseStored(rows.front()[0].value().value_or("{}"),
                                                             {.resource = context.resource()});
    const auto runtime =
        parseStored(rows.front()[1].value().value_or("{}"), {.resource = context.resource()});
    if (!config || !runtime) {
        throw std::runtime_error("网站运行状态损坏");
    }

    const auto expectedTarget = rows.front()[2].value().value_or("");
    const auto checkedAt = rows.front()[5].value().value_or("");
    auto output = co_await probeWebsiteDomains(context, *config, expectedTarget, checkedAt, marker);

    const auto json = ruvia::toJson(toOutput(output, {.resource = context.resource()}),
                                    {.resource = context.resource()});
    auto completion = co_await context.db().beginTransaction();
    const auto updated = co_await completion.execute(
        "UPDATE sys_website SET runtime = $2::jsonb WHERE id = $1 AND revision = $3 AND "
        "tenant_id = $4 AND deleted_at IS NULL",
        websiteId, std::string_view(json), expectedRevision, marker.marker.tenantId);
    service::sync_runtime::RunningResultTransition resultTransition;
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(completion, marker);
    } else {
        if (!co_await service::sync_runtime::renewRunningLease(completion, marker)) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
        resultTransition =
            co_await service::sync_runtime::completeRunningAndRecordEvent(completion, marker);
        if (!resultTransition.markerTransitioned) {
            throw std::runtime_error("网站同步标记 lease 已失效");
        }
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(completion, marker,
                                                                resultTransition);
    co_return;
}

} // namespace service::website_dns
