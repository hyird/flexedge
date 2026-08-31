#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/cluster/cluster.error.h"
#include "service/domains/cluster/cluster.types.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/website_dns/projection.h"

namespace service::cluster {

class ClusterService {
  public:
    ruvia::Task<ClusterPageDataDto>
    list(ruvia::Context& c, const std::string& tenantId, std::int64_t page, std::int64_t pageSize,
         std::int64_t skip, const std::optional<std::string>& keyword,
         const std::optional<std::string>& dnsZoneId, const std::optional<std::string>& status) {
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

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const SaveClusterBody& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "集群配置不能为空", 400);
        }
        const auto name = trim(normalized->name);
        const auto& dnsZoneId = normalized->dnsZoneId;
        const auto hostnamePrefix = normalizePrefix(normalized->hostnamePrefix);
        const auto& status = normalized->status;
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto zoneDomain = co_await requireZone(transaction, tenantId, dnsZoneId);
            if ((hostnamePrefix + "." + zoneDomain).size() > 253) {
                service::common::throwAppError(ClusterError::DNS_ZONE_UNAVAILABLE);
            }
            const auto rows = co_await transaction.query(
                "INSERT INTO sys_cluster (tenant_id, dns_zone_id, name, hostname_prefix, "
                "status, revision, created_at, updated_at) VALUES ($1, $2, $3, $4, $5, 1, "
                "NOW(), NOW()) RETURNING id",
                service::common::dbParams(ruvia::DbValue{tenantId}, ruvia::DbValue{dnsZoneId},
                                          ruvia::DbValue{std::string_view(name)},
                                          ruvia::DbValue{std::string_view(hostnamePrefix)},
                                          ruvia::DbValue{status}));
            co_await service::node_dispatch::publishClusterRelease(
                transaction, tenantId, rows.front()[0].value().value_or(""));
            co_await transaction.commit();
        } catch (const ruvia::DbError& error) {
            if (isIdentityConflict(error)) {
                service::common::throwAppError(ClusterError::EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> update(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision, const SaveClusterBody& body) {
        const auto normalized = normalize(body);
        if (!normalized) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "集群配置不能为空", 400);
        }
        const auto name = trim(normalized->name);
        const auto& dnsZoneId = normalized->dnsZoneId;
        const auto hostnamePrefix = normalizePrefix(normalized->hostnamePrefix);
        const auto& status = normalized->status;
        try {
            auto transaction = co_await c.db().beginTransaction();
            const auto zoneDomain = co_await requireZone(transaction, tenantId, dnsZoneId);
            if ((hostnamePrefix + "." + zoneDomain).size() > 253) {
                service::common::throwAppError(ClusterError::DNS_ZONE_UNAVAILABLE);
            }
            const auto rows = co_await transaction.query(
                "WITH current AS (SELECT id, dns_zone_id AS previous_dns_zone_id FROM "
                "sys_cluster WHERE id = $5 AND tenant_id = $6 AND revision = $7 AND "
                "deleted_at IS NULL FOR UPDATE) UPDATE sys_cluster cluster SET dns_zone_id = "
                "$1, name = $2, hostname_prefix = $3, status = $4, revision = cluster.revision + "
                "1, updated_at = NOW() FROM current WHERE cluster.id = current.id RETURNING "
                "current.previous_dns_zone_id, cluster.dns_zone_id",
                service::common::dbParams(
                    ruvia::DbValue{dnsZoneId}, ruvia::DbValue{std::string_view(name)},
                    ruvia::DbValue{std::string_view(hostnamePrefix)}, ruvia::DbValue{status},
                    ruvia::DbValue{id}, ruvia::DbValue{tenantId},
                    ruvia::DbValue{expectedRevision}));
            if (rows.empty()) {
                const auto current = co_await transaction.query(
                    "SELECT revision FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND "
                    "deleted_at IS NULL",
                    id, tenantId);
                if (current.empty()) {
                    service::common::throwAppError(ClusterError::NOT_FOUND);
                }
                service::common::throwAppError(ClusterError::REVISION_CONFLICT);
            }
            co_await service::cluster_dns::reconcileZoneTransition(
                transaction, tenantId, std::string(rows.front()[0].value().value_or("")),
                std::string(rows.front()[1].value().value_or("")));
            co_await service::website_dns::reconcileClusterConsumers(transaction, tenantId, id);
            co_await service::node_dispatch::publishClusterRelease(transaction, tenantId, id);
            co_await transaction.commit();
        } catch (const ruvia::DbError& error) {
            if (isIdentityConflict(error)) {
                service::common::throwAppError(ClusterError::EXISTS);
            }
            throw;
        }
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto clusters = co_await transaction.query(
            "SELECT cluster.revision, EXISTS(SELECT 1 FROM sys_node node WHERE node.tenant_id = "
            "cluster.tenant_id AND node.cluster_id = cluster.id AND node.deleted_at IS NULL), "
            "EXISTS(SELECT 1 FROM sys_website website WHERE website.tenant_id = "
            "cluster.tenant_id AND website.cluster_id = cluster.id AND website.deleted_at IS "
            "NULL), EXISTS(SELECT 1 FROM sys_website website INNER JOIN sys_sync_task task ON "
            "task.tenant_id = website.tenant_id AND task.resource_type = 'website' AND "
            "task.resource_id = website.id AND task.operation = 'delete' AND NOT task.is_done "
            "WHERE "
            "website.tenant_id = cluster.tenant_id AND website.cluster_id = cluster.id AND "
            "website.deleted_at IS NOT NULL) FROM "
            "sys_cluster cluster WHERE cluster.id = $1 AND cluster.tenant_id = $2 AND "
            "cluster.deleted_at IS NULL LIMIT 1 FOR UPDATE OF cluster",
            service::common::dbParams(ruvia::DbValue{id}, ruvia::DbValue{tenantId}));
        if (clusters.empty()) {
            service::common::throwAppError(ClusterError::NOT_FOUND);
        }
        if (clusters.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(ClusterError::REVISION_CONFLICT);
        }
        if (clusters.front()[1].as<bool>().value_or(false)) {
            service::common::throwAppError(ClusterError::HAS_NODES);
        }
        if (clusters.front()[2].as<bool>().value_or(false)) {
            service::common::throwAppError(ClusterError::HAS_WEBSITES);
        }
        if (clusters.front()[3].as<bool>().value_or(false)) {
            service::common::throwAppError(ClusterError::WEBSITE_CLEANUP_ACTIVE);
        }
        const auto rows = co_await transaction.query(
            "UPDATE sys_cluster SET revision = revision + 1, deleted_at = NOW(), updated_at = "
            "NOW() WHERE id = $1 AND tenant_id = $2 AND revision = $3 AND deleted_at IS NULL "
            "RETURNING id",
            service::common::dbParams(ruvia::DbValue{id}, ruvia::DbValue{tenantId},
                                      ruvia::DbValue{expectedRevision}));
        if (rows.empty()) {
            service::common::throwAppError(ClusterError::NOT_FOUND);
        }
        co_await service::cluster_dns::reconcileCluster(transaction, tenantId, id);
        co_await transaction.commit();
        co_return;
    }

  private:
    static ruvia::Task<std::string> requireZone(ruvia::DbTransaction& transaction,
                                                const std::string& tenantId,
                                                const std::string& dnsZoneId) {
        const auto rows = co_await transaction.query(
            "SELECT zone.domain FROM sys_dns_zone zone INNER JOIN sys_provider provider ON "
            "provider.tenant_id = zone.tenant_id AND provider.id = zone.provider_id AND "
            "provider.kind = 'dns' WHERE zone.id = $1 AND zone.tenant_id = $2 AND "
            "zone.deleted_at IS NULL AND provider.deleted_at IS NULL LIMIT 1 FOR SHARE OF zone, "
            "provider",
            service::common::dbParams(ruvia::DbValue{dnsZoneId}, ruvia::DbValue{tenantId}));
        if (rows.empty()) {
            service::common::throwAppError(ClusterError::DNS_ZONE_UNAVAILABLE);
        }
        co_return std::string(rows.front()[0].value().value_or(""));
    }

    static std::string trim(std::string_view input) {
        const auto begin = std::find_if_not(input.begin(), input.end(),
                                            [](unsigned char ch) { return std::isspace(ch) != 0; });
        const auto end = std::find_if_not(input.rbegin(), input.rend(), [](unsigned char ch) {
                             return std::isspace(ch) != 0;
                         }).base();
        return begin < end ? std::string(begin, end) : std::string{};
    }

    static std::string normalizePrefix(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    static bool isIdentityConflict(const ruvia::DbError& error) {
        return service::common::isUniqueConstraintViolation(error, "uk_cluster_name") ||
               service::common::isUniqueConstraintViolation(error, "uk_cluster_hostname");
    }
};

inline ClusterService& clusterService() {
    static ClusterService service;
    return service;
}

} // namespace service::cluster
