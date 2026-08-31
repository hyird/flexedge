#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/node_config/model.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync {

struct SnapshotRecord final {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string lineCode;
};

inline bool isClusterManagedRecord(const ZoneRecordData& record,
                                   const std::unordered_set<std::string>& hostnames) {
    return hostnames.contains(record.name) &&
           (record.type == "A" || record.type == "AAAA" || record.type == "CNAME");
}

using ProjectedRecordsByZone = std::unordered_map<std::string, std::vector<SnapshotRecord>>;

template <typename Db>
inline ruvia::Task<ProjectedRecordsByZone>
loadProjectedRecordsByZone(Db& db, const std::string& tenantId,
                           const std::vector<std::string>& zoneIds) {
    ProjectedRecordsByZone result;
    if (zoneIds.empty()) {
        co_return result;
    }

    std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
    std::string placeholders;
    for (const auto& zoneId : zoneIds) {
        result.try_emplace(zoneId);
        if (!placeholders.empty()) {
            placeholders += ", ";
        }
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(zoneId);
    }

    const auto nodeRows = co_await db.query(
        "SELECT cluster.dns_zone_id, node.config::text, cluster.hostname_prefix FROM sys_node "
        "node INNER JOIN sys_cluster cluster ON cluster.tenant_id = node.tenant_id AND "
        "cluster.id = node.cluster_id INNER JOIN sys_dns_zone managed_zone ON "
        "managed_zone.tenant_id = cluster.tenant_id AND managed_zone.id = "
        "cluster.dns_zone_id WHERE node.tenant_id = $1 AND cluster.dns_zone_id IN (" +
            placeholders +
            ") AND node.registration_status = 'registered' AND node.status = 'enabled' AND "
            "node.deleted_at IS NULL AND cluster.deleted_at IS NULL AND cluster.status = "
            "'enabled' AND managed_zone.deleted_at IS NULL ORDER BY node.sort ASC",
        params);
    for (const auto& row : nodeRows) {
        const auto zoneId = std::string(row[0].value().value_or(""));
        const std::optional<service::node_config::NodeConfigData> config =
            service::node_config::parseStored(row[1].value().value_or("{}"));
        if (!config) {
            throw std::runtime_error("节点聚合配置损坏");
        }
        auto& records = result.at(zoneId);
        for (const auto& endpoint : config->endpoints) {
            const auto& address = endpoint.ipAddress;
            records.push_back({
                .id = endpoint.id,
                .type = address.contains(':') ? "AAAA" : "A",
                .name = std::string(row[2].value().value_or("")),
                .content = address,
                .ttl = 600,
                .priority = std::nullopt,
                .proxied = false,
                .lineCode = endpoint.lineCode,
            });
        }
    }

    const auto websiteRows = co_await db.query(
        "SELECT claim.dns_zone_id, claim.domain_id, claim.domain_key, "
        "cluster.hostname_prefix || '.' || access_zone.domain FROM "
        "sys_website_domain_claim claim INNER JOIN sys_website website ON website.tenant_id = "
        "claim.tenant_id AND website.id = claim.website_id INNER JOIN sys_cluster cluster ON "
        "cluster.tenant_id = website.tenant_id AND cluster.id = website.cluster_id INNER "
        "JOIN sys_dns_zone access_zone ON access_zone.tenant_id = cluster.tenant_id AND "
        "access_zone.id = cluster.dns_zone_id INNER JOIN sys_dns_zone managed_zone ON "
        "managed_zone.tenant_id = claim.tenant_id AND managed_zone.id = claim.dns_zone_id "
        "WHERE claim.tenant_id = $1 AND claim.dns_zone_id IN (" +
            placeholders +
            ") AND claim.dns_mode = 'managed' AND website.status = 'enabled' AND "
            "website.deleted_at IS NULL AND cluster.status = 'enabled' AND cluster.deleted_at IS "
            "NULL AND access_zone.deleted_at IS NULL AND managed_zone.deleted_at IS NULL ORDER BY "
            "website.sort ASC, claim.domain_key ASC",
        params);
    for (const auto& row : websiteRows) {
        result.at(std::string(row[0].value().value_or("")))
            .push_back({
                .id = std::string(row[1].value().value_or("")),
                .type = "CNAME",
                .name = std::string(row[2].value().value_or("")),
                .content = std::string(row[3].value().value_or("")),
                .ttl = 600,
                .priority = std::nullopt,
                .proxied = false,
                .lineCode = "default",
            });
    }
    co_return result;
}

template <typename Db>
inline ruvia::Task<std::vector<SnapshotRecord>>
loadProjectedRecords(Db& db, const std::string& tenantId, const std::string& zoneId) {
    auto records = co_await loadProjectedRecordsByZone(db, tenantId, std::vector{zoneId});
    co_return std::move(records.at(zoneId));
}

inline ruvia::Task<std::string> enqueueZoneRevision(ruvia::DbTransaction& transaction,
                                                    const std::string& tenantId,
                                                    const std::string& zoneId,
                                                    std::int64_t revision,
                                                    std::string_view operation = "sync") {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "(deleted_at IS NULL OR $3::text = 'delete') LIMIT 1",
        tenantId, zoneId, operation);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
    co_return co_await service::sync_runtime::upsertMarker(transaction, tenantId, "dns_zone",
                                                           zoneId, operation, revision);
}

inline ruvia::Task<std::string> enqueueZoneDeletion(ruvia::DbTransaction& transaction,
                                                    const std::string& tenantId,
                                                    const std::string& zoneId,
                                                    std::int64_t revision) {
    co_return co_await enqueueZoneRevision(transaction, tenantId, zoneId, revision, "delete");
}

inline ruvia::Task<std::optional<std::string>>
pruneClusterManagedRecords(ruvia::DbTransaction& transaction, const std::string& tenantId,
                           const std::string& zoneId, std::string_view operation = "sync") {
    const auto zoneRows = co_await transaction.query(
        "SELECT revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenantId, zoneId);
    if (zoneRows.empty()) {
        co_return std::nullopt;
    }
    const auto clusterRows = co_await transaction.query(
        "SELECT hostname_prefix FROM sys_cluster WHERE tenant_id = $1 AND dns_zone_id = $2 AND "
        "status = 'enabled' AND deleted_at IS NULL",
        tenantId, zoneId);
    if (clusterRows.empty()) {
        co_return std::nullopt;
    }

    std::unordered_set<std::string> hostnames;
    hostnames.reserve(clusterRows.size());
    for (const auto& row : clusterRows) {
        hostnames.emplace(row[0].value().value_or(""));
    }

    const auto configRows = co_await transaction.query(
        "SELECT config::text FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenantId,
        zoneId);
    const std::optional<ZoneConfigData> config =
        parseStored(configRows.front()[0].value().value_or("{}"));
    if (!config) {
        throw std::runtime_error("DNS 聚合配置损坏");
    }

    ZoneConfigOutput filtered;
    auto& filteredRecords = filtered.ensure<"records">();
    bool changed = false;
    for (const auto& record : config->records) {
        if (isClusterManagedRecord(record, hostnames)) {
            changed = true;
            continue;
        }
        auto& output = filteredRecords.emplace_back();
        output.set<"id">(record.id);
        output.set<"type">(record.type);
        output.set<"name">(record.name);
        output.set<"content">(record.content);
        output.set<"ttl">(record.ttl);
        output.set<"proxied">(record.proxied);
        output.set<"lineCode">(record.lineCode);
        if (record.priority) {
            output.set<"priority">(*record.priority);
        }
    }
    if (!changed) {
        co_return std::nullopt;
    }

    const auto configJson = ruvia::toJson(filtered);
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, revision = revision + 1, "
        "desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE tenant_id = $2 AND id = $3 AND revision = $4 AND "
        "deleted_at IS NULL RETURNING desired_revision",
        std::string_view(configJson), tenantId, zoneId,
        zoneRows.front()[0].as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        co_return std::nullopt;
    }
    co_return co_await enqueueZoneRevision(transaction, tenantId, zoneId,
                                           updated.front()[0].as<std::int64_t>().value_or(1),
                                           operation);
}

inline ruvia::Task<std::string> markZoneDirty(ruvia::DbTransaction& transaction,
                                              const std::string& tenantId,
                                              const std::string& zoneId,
                                              std::string_view operation = "sync") {
    if (const auto pruned =
            co_await pruneClusterManagedRecords(transaction, tenantId, zoneId, operation)) {
        co_return *pruned;
    }
    const auto rows = co_await transaction.query(
        "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
        "'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL RETURNING desired_revision",
        tenantId, zoneId);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
    co_return co_await enqueueZoneRevision(
        transaction, tenantId, zoneId, rows.front()[0].as<std::int64_t>().value_or(1), operation);
}

inline ruvia::Task<void> markProviderZonesDirty(ruvia::DbTransaction& transaction,
                                                const std::string& tenantId,
                                                const std::string& providerId) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
        "'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $1 AND "
        "provider_id = $2 AND deleted_at IS NULL RETURNING id, desired_revision",
        tenantId, providerId);
    for (const auto& row : rows) {
        co_await enqueueZoneRevision(transaction, tenantId,
                                     std::string(row[0].value().value_or("")),
                                     row[1].as<std::int64_t>().value_or(1));
    }
    co_return;
}

} // namespace service::dns_sync
