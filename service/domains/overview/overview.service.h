#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/domains/overview/overview.types.h"

namespace service::overview {

class OverviewService final {
  public:
    ruvia::Task<OverviewDataDto> get(ruvia::Context& c, const std::string& tenantId) {
        const auto summaryRows = co_await c.db().query(
            "SELECT "
            "(SELECT COUNT(*) FROM sys_website website WHERE website.tenant_id = $1 AND "
            "website.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_website_domain_claim claim INNER JOIN sys_website "
            "website ON website.tenant_id = claim.tenant_id AND website.id = claim.website_id "
            "WHERE claim.tenant_id = $1 AND website.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_certificate certificate WHERE certificate.tenant_id = $1 "
            "AND certificate.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_cluster cluster WHERE cluster.tenant_id = $1 AND "
            "cluster.deleted_at IS NULL), "
            "(SELECT COUNT(*) FROM sys_dns_zone zone WHERE zone.tenant_id = $1 AND "
            "zone.deleted_at IS NULL AND zone.sync_status IN ('failed', 'conflict')), "
            "(SELECT COUNT(*) FROM sys_certificate certificate WHERE certificate.tenant_id = $1 "
            "AND certificate.deleted_at IS NULL AND certificate.status = 'valid' AND "
            "certificate.expires_at > NOW() AND certificate.expires_at <= NOW() + INTERVAL '30 "
            "days'), "
            "(SELECT COUNT(*) FROM sys_certificate certificate WHERE certificate.tenant_id = $1 "
            "AND certificate.deleted_at IS NULL AND (certificate.status IN ('failed', "
            "'expired') OR (certificate.status = 'valid' AND certificate.last_error IS NOT "
            "NULL))), "
            "(SELECT COUNT(*) FROM sys_sync_task marker WHERE marker.tenant_id = $1 AND "
            "NOT marker.is_done), "
            "(SELECT COUNT(*) FROM sys_sync_task marker WHERE marker.tenant_id = $1 AND "
            "NOT marker.is_done AND marker.count_fails > 0)",
            tenantId);

        OverviewDataDto result(c);
        OverviewResourceCountsDto resources(c);
        OverviewIssueCountsDto issues(c);
        if (summaryRows.empty()) {
            resources.set<"websiteCount">(0);
            resources.set<"domainCount">(0);
            resources.set<"certificateCount">(0);
            resources.set<"clusterCount">(0);
            issues.set<"dnsZoneIssueCount">(0);
            issues.set<"certificateExpiringCount">(0);
            issues.set<"certificateFailedCount">(0);
            issues.set<"activeTaskCount">(0);
            issues.set<"failedTaskCount">(0);
        } else {
            const auto& row = summaryRows.front();
            resources.set<"websiteCount">(count(row, 0));
            resources.set<"domainCount">(count(row, 1));
            resources.set<"certificateCount">(count(row, 2));
            resources.set<"clusterCount">(count(row, 3));
            issues.set<"dnsZoneIssueCount">(count(row, 4));
            issues.set<"certificateExpiringCount">(count(row, 5));
            issues.set<"certificateFailedCount">(count(row, 6));
            issues.set<"activeTaskCount">(count(row, 7));
            issues.set<"failedTaskCount">(count(row, 8));
        }
        result.set<"resources">(std::move(resources));
        result.set<"issues">(std::move(issues));

        const auto taskRows = co_await c.db().query(
            "SELECT marker.id, CASE marker.resource_type WHEN 'dns_zone' THEN 'dns' ELSE "
            "marker.resource_type END, marker.resource_type, marker.resource_id, CASE "
            "marker.resource_type WHEN 'provider' THEN COALESCE((SELECT CASE provider.kind WHEN "
            "'dns' THEN COALESCE(provider.name, provider.provider) ELSE provider.provider END "
            "FROM sys_provider provider WHERE provider.tenant_id = marker.tenant_id AND "
            "provider.id = marker.resource_id), '供应商') WHEN 'dns_zone' THEN COALESCE((SELECT "
            "zone.domain FROM sys_dns_zone zone WHERE zone.tenant_id = marker.tenant_id AND "
            "zone.id = marker.resource_id), 'DNS 托管域名') WHEN 'certificate' THEN "
            "COALESCE((SELECT "
            "certificate.domain FROM sys_certificate certificate WHERE certificate.tenant_id = "
            "marker.tenant_id AND certificate.id = marker.resource_id), '证书') WHEN 'website' "
            "THEN "
            "COALESCE((SELECT MIN(claim.domain_key) FROM sys_website_domain_claim claim WHERE "
            "claim.tenant_id = marker.tenant_id AND claim.website_id = marker.resource_id), "
            "'网站') "
            "ELSE marker.resource_type END, marker.operation, CASE WHEN marker.lease_until IS NOT "
            "NULL THEN 'running' WHEN marker.is_done AND marker.is_ok THEN 'completed' WHEN "
            "marker.count_fails > 0 THEN 'retry' ELSE 'pending' "
            "END, "
            "CASE WHEN marker.count_fails > 0 AND marker.error <> '' THEN marker.error ELSE NULL "
            "END, "
            "TO_CHAR(marker.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_sync_task "
            "marker "
            "WHERE marker.tenant_id = $1 ORDER BY marker.updated_at DESC, marker.id DESC LIMIT 8",
            tenantId);
        auto& recentTasks = result.ensure<"recentTasks">();
        for (const auto& row : taskRows) {
            auto& task = recentTasks.emplace_back(c);
            task.set<"id">(row[0].value().value_or(""));
            task.set<"kind">(row[1].value().value_or(""));
            task.set<"resourceType">(row[2].value().value_or(""));
            task.set<"resourceId">(row[3].value().value_or(""));
            task.set<"resourceName">(row[4].value().value_or(""));
            task.set<"operation">(row[5].value().value_or(""));
            task.set<"status">(row[6].value().value_or("pending"));
            task.set<"updatedAt">(row[8].value().value_or(""));
            if (const auto& lastError = row[7].value()) {
                task.set<"lastError">(*lastError);
            }
        }
        co_return result;
    }

  private:
    template <typename Row> static std::int64_t count(const Row& row, std::size_t index) {
        return row[index].template as<std::int64_t>().value_or(0);
    }
};

inline OverviewService& overviewService() {
    static OverviewService instance;
    return instance;
}

} // namespace service::overview
