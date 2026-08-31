#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/http.h"
#include "service/domains/task/task.types.h"

namespace service::task {

class TaskService final {
  public:
    ruvia::Task<TaskPageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                      std::int64_t page, std::int64_t pageSize, std::int64_t skip,
                                      const std::optional<std::string>& status,
                                      const std::optional<std::string>& resourceType,
                                      const std::optional<std::string>& resourceId) {
        std::string where = " WHERE tenant_id = $1";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        if (resourceType && resourceId) {
            where += " AND resource_type = $" + std::to_string(params.size() + 1) +
                     " AND resource_id = $" + std::to_string(params.size() + 2);
            params.emplace_back(std::string_view(*resourceType));
            params.emplace_back(*resourceId);
        }

        const auto markerView = view();
        const auto summaryRows =
            co_await c.db().query("WITH marker_view AS (" + markerView +
                                      ") SELECT "
                                      "COUNT(*) FILTER (WHERE status = 'pending'), COUNT(*) FILTER "
                                      "(WHERE status = 'running'), "
                                      "COUNT(*) FILTER (WHERE status = 'retry'), COUNT(*) FILTER "
                                      "(WHERE status = 'completed') FROM marker_view" +
                                      where,
                                  params);

        if (status) {
            where += " AND status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }
        const auto countRows = co_await c.db().query(
            "WITH marker_view AS (" + markerView + ") SELECT COUNT(*) FROM marker_view" + where,
            params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "WITH marker_view AS (" + markerView +
                ") SELECT id, kind, resource_type, "
                "resource_id, operation, resource_name, status, version, processed_version, "
                "count_fails, TO_CHAR(next_attempt_at, "
                "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
                "TO_CHAR(lease_until, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), CASE WHEN status = "
                "'retry' THEN marker_error ELSE NULL END, CASE WHEN status = "
                "'completed' THEN TO_CHAR(updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') "
                "ELSE NULL END, TO_CHAR(created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
                "TO_CHAR(updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM marker_view" +
                where + " ORDER BY updated_at DESC, id DESC LIMIT " + std::to_string(pageSize) +
                " OFFSET " + std::to_string(skip),
            params);

        TaskPageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        TaskSummaryDto summary(c);
        if (summaryRows.empty()) {
            summary.set<"pending">(0);
            summary.set<"running">(0);
            summary.set<"retry">(0);
            summary.set<"completed">(0);
        } else {
            const auto& row = summaryRows.front();
            summary.set<"pending">(row[0].as<std::int64_t>().value_or(0));
            summary.set<"running">(row[1].as<std::int64_t>().value_or(0));
            summary.set<"retry">(row[2].as<std::int64_t>().value_or(0));
            summary.set<"completed">(row[3].as<std::int64_t>().value_or(0));
        }
        result.set<"summary">(std::move(summary));

        auto& items = result.ensure<"list">();
        std::int64_t sequence = skip;
        for (const auto& row : rows) {
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"sequence">(++sequence);
            item.set<"kind">(row[1].value().value_or(""));
            item.set<"resourceType">(row[2].value().value_or(""));
            item.set<"resourceId">(row[3].value().value_or(""));
            item.set<"operation">(row[4].value().value_or(""));
            item.set<"resourceName">(row[5].value().value_or(""));
            item.set<"status">(row[6].value().value_or("pending"));
            item.set<"version">(row[7].as<std::int64_t>().value_or(0));
            item.set<"processedVersion">(row[8].as<std::int64_t>().value_or(0));
            item.set<"countFails">(row[9].as<std::int64_t>().value_or(0));
            item.set<"nextAttemptAt">(row[10].value().value_or(""));
            if (const auto& leaseUntil = row[11].value()) {
                item.set<"leaseUntil">(*leaseUntil);
            }
            if (const auto& error = row[12].value()) {
                item.set<"error">(*error);
            }
            if (const auto& completedAt = row[13].value()) {
                item.set<"completedAt">(*completedAt);
            }
            item.set<"createdAt">(row[14].value().value_or(""));
            item.set<"updatedAt">(row[15].value().value_or(""));
        }
        co_return result;
    }

  private:
    static std::string view() {
        return "SELECT marker.id, CASE marker.resource_type WHEN 'dns_zone' THEN 'dns' ELSE "
               "marker.resource_type END AS kind, marker.resource_type, marker.resource_id, "
               "marker.operation, CASE marker.resource_type WHEN 'provider' THEN COALESCE((SELECT "
               "CASE provider.kind WHEN 'dns' THEN COALESCE(provider.name, provider.provider) ELSE "
               "provider.provider END FROM sys_provider provider WHERE provider.tenant_id = "
               "marker.tenant_id AND provider.id = marker.resource_id), '供应商') WHEN 'dns_zone' "
               "THEN COALESCE((SELECT zone.domain FROM sys_dns_zone zone WHERE zone.tenant_id = "
               "marker.tenant_id AND zone.id = marker.resource_id), 'DNS 托管域名') WHEN "
               "'certificate' THEN COALESCE((SELECT certificate.domain FROM sys_certificate "
               "certificate WHERE certificate.tenant_id = marker.tenant_id AND certificate.id = "
               "marker.resource_id), '证书') WHEN 'website' THEN COALESCE((SELECT "
               "MIN(claim.domain_key) "
               "FROM sys_website_domain_claim claim WHERE claim.tenant_id = marker.tenant_id AND "
               "claim.website_id = marker.resource_id), '网站') ELSE marker.resource_type END AS "
               "resource_name, CASE WHEN marker.lease_until IS NOT NULL THEN 'running' WHEN "
               "marker.is_done AND marker.is_ok THEN 'completed' "
               "WHEN marker.count_fails > 0 THEN 'retry' ELSE 'pending' END AS status, "
               "marker.version, "
               "marker.processed_version, marker.count_fails, "
               "marker.next_attempt_at, marker.lease_until, marker.error AS "
               "marker_error, "
               "marker.created_at, marker.updated_at, marker.tenant_id FROM sys_sync_task marker";
    }
};

inline TaskService& taskService() {
    static TaskService instance;
    return instance;
}

} // namespace service::task
