#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>

#include "service/common/http.h"
#include "service/domains/cluster/cluster.types.h"

namespace service::cluster {

class ClusterReadService final {
  public:
    ruvia::Task<ClusterPageDataDto> list(ruvia::Context& c, const std::string& tenantId,
                                         std::int64_t page, std::int64_t pageSize,
                                         std::int64_t skip,
                                         const std::optional<std::string>& keyword,
                                         const std::optional<std::string>& dnsZoneId,
                                         const std::optional<std::string>& status) const {
        std::string where =
            " FROM sys_cluster cluster INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
            "cluster.tenant_id AND zone.id = cluster.dns_zone_id INNER JOIN sys_provider "
            "provider ON provider.tenant_id = zone.tenant_id AND provider.id = "
            "zone.provider_id AND provider.kind = 'dns' WHERE cluster.tenant_id = $1 AND "
            "cluster.deleted_at IS NULL AND zone.deleted_at IS NULL AND provider.deleted_at IS "
            "NULL";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> keywordPattern;
        if (keyword) {
            keywordPattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            const auto placeholder = "$" + std::to_string(params.size() + 1);
            where += " AND (cluster.name ILIKE " + placeholder +
                     " OR (cluster.hostname_prefix || '.' || zone.domain) ILIKE " + placeholder +
                     ")";
            params.emplace_back(std::string_view(*keywordPattern));
        }
        if (dnsZoneId) {
            where += " AND cluster.dns_zone_id = $" + std::to_string(params.size() + 1);
            params.emplace_back(*dnsZoneId);
        }
        if (status) {
            where += " AND cluster.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*status));
        }

        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows = co_await c.db().query(
            "SELECT cluster.id, cluster.name, cluster.dns_zone_id, zone.domain, provider.provider, "
            "cluster.hostname_prefix, cluster.hostname_prefix || '.' || zone.domain, "
            "(SELECT COUNT(*) FROM sys_node node WHERE node.tenant_id = cluster.tenant_id "
            "AND node.cluster_id = cluster.id AND node.deleted_at IS NULL), (SELECT COUNT(*) FROM "
            "sys_node node WHERE node.tenant_id = cluster.tenant_id AND node.cluster_id = "
            "cluster.id AND node.deleted_at IS NULL AND node.registration_status = 'registered' "
            "AND "
            "node.status = 'enabled' AND node.last_heartbeat_at >= NOW() - INTERVAL '90 "
            "seconds'), cluster.status, cluster.revision, "
            "TO_CHAR(cluster.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
            "TO_CHAR(cluster.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF')" +
                where + " ORDER BY cluster.sort DESC LIMIT " + std::to_string(pageSize) +
                " OFFSET " + std::to_string(skip),
            params);

        ClusterPageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            auto& item = items.emplace_back(c);
            item.set<"id">(row[0].value().value_or(""));
            item.set<"name">(row[1].value().value_or(""));
            item.set<"dnsZoneId">(row[2].value().value_or(""));
            item.set<"dnsZoneDomain">(row[3].value().value_or(""));
            item.set<"dnsProviderName">(row[4].value().value_or(""));
            item.set<"hostnamePrefix">(row[5].value().value_or(""));
            item.set<"accessDomain">(row[6].value().value_or(""));
            item.set<"nodeCount">(row[7].as<std::int64_t>().value_or(0));
            item.set<"onlineNodeCount">(row[8].as<std::int64_t>().value_or(0));
            item.set<"status">(row[9].value().value_or("disabled"));
            item.set<"revision">(row[10].as<std::int64_t>().value_or(1));
            item.set<"createdAt">(row[11].value().value_or(""));
            item.set<"updatedAt">(row[12].value().value_or(""));
        }
        co_return result;
    }
};

inline const ClusterReadService& clusterReadService() {
    static const ClusterReadService service;
    return service;
}

} // namespace service::cluster
