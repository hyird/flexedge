#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "service/common/database.h"
#include "service/common/domain_name.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/node_config/mapper.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync {

// Who projects a system record: the nodes of a cluster (A/AAAA at its access hostname) or a
// hosted website domain (CNAME to its cluster's access hostname).
enum class snapshot_record_source { cluster,
    website };

struct snapshot_record final {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    bool proxied_;
    std::string line_code_;
    snapshot_record_source source_;
};

inline bool is_cluster_managed_record(const zone_record_data& record,
    const std::unordered_set<std::string>& hostnames) {
    return hostnames.contains(record.name_) &&
           (record.type_ == "A" || record.type_ == "AAAA" || record.type_ == "CNAME");
}

inline std::string record_hostname(std::string_view name, std::string_view zone_domain) {
    const auto normalized_name = service::common::normalize_domain_name(name);
    const auto normalized_zone = service::common::normalize_domain_name(zone_domain);
    if (normalized_name == "@" || normalized_name == normalized_zone) {
        return normalized_zone;
    }
    if (normalized_name.ends_with("." + normalized_zone)) {
        return normalized_name;
    }
    return normalized_name + "." + normalized_zone;
}

inline bool is_traffic_record(std::string_view type) {
    return type == "A" || type == "AAAA" || type == "CNAME";
}

using projected_records_by_zone = std::unordered_map<std::string, std::vector<snapshot_record>>;

template <typename db_type>
inline ruvia::task<projected_records_by_zone>
load_projected_records_by_zone(db_type& db, const std::string& tenant_id,
    const std::vector<std::string>& zone_ids) {
    projected_records_by_zone result;
    if (zone_ids.empty()) {
        co_return result;
    }

    std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
    std::string placeholders;
    for (const auto& zone_id : zone_ids) {
        result.try_emplace(zone_id);
        if (!placeholders.empty()) {
            placeholders += ", ";
        }
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(zone_id);
    }

    const auto node_rows = co_await db.query(
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
    for (const auto& row : node_rows) {
        const auto zone_id = std::string(row[0].value().value_or(""));
        const std::optional<service::node_config::node_config_data> config =
            service::node_config::parse_stored(row[1].value().value_or("{}"));
        if (!config) {
            throw std::runtime_error("节点聚合配置损坏");
        }
        auto& records = result.at(zone_id);
        for (const auto& endpoint : config->endpoints_) {
            const auto& address = endpoint.ip_address_;
            records.push_back({
                .id_ = endpoint.id_,
                .type_ = address.find(':') != std::string::npos ? "AAAA" : "A",
                .name_ = std::string(row[2].value().value_or("")),
                .content_ = address,
                .ttl_ = 600,
                .priority_ = std::nullopt,
                .proxied_ = false,
                .line_code_ = endpoint.line_code_,
                .source_ = snapshot_record_source::cluster,
            });
        }
    }

    const auto website_rows = co_await db.query(
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
    for (const auto& row : website_rows) {
        result.at(std::string(row[0].value().value_or("")))
            .push_back({
                .id_ = std::string(row[1].value().value_or("")),
                .type_ = "CNAME",
                .name_ = std::string(row[2].value().value_or("")),
                .content_ = std::string(row[3].value().value_or("")),
                .ttl_ = 600,
                .priority_ = std::nullopt,
                .proxied_ = false,
                .line_code_ = "default",
                .source_ = snapshot_record_source::website,
            });
    }
    co_return result;
}

template <typename db_type>
inline ruvia::task<std::vector<snapshot_record>>
load_projected_records(db_type& db, const std::string& tenant_id, const std::string& zone_id) {
    auto records = co_await load_projected_records_by_zone(db, tenant_id, std::vector{zone_id});
    co_return std::move(records.at(zone_id));
}

inline ruvia::task<std::string>
enqueue_zone_revision(ruvia::db_transaction& transaction, const std::string& tenant_id,
    const std::string& zone_id, std::int64_t revision, service::operation::work_origin& origin,
    service::sync_runtime::marker_operation operation =
        service::sync_runtime::marker_operation::sync) {
    const auto operation_value = service::sync_runtime::marker_operation_name(operation);
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "(deleted_at IS NULL OR $3::text = 'delete') LIMIT 1",
        tenant_id, zone_id, operation_value);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
    co_return co_await service::sync_runtime::upsert_marker(
        transaction, tenant_id, service::sync_runtime::marker_resource_type::dns_zone, zone_id,
        operation, revision, origin);
}

inline ruvia::task<std::string> enqueue_zone_deletion(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& zone_id,
    std::int64_t revision, service::operation::work_origin& origin) {
    co_return co_await enqueue_zone_revision(transaction, tenant_id, zone_id, revision, origin,
        service::sync_runtime::marker_operation::remove);
}

inline ruvia::task<std::optional<std::string>>
prune_cluster_managed_records(ruvia::db_transaction& transaction, const std::string& tenant_id,
    const std::string& zone_id, service::operation::work_origin& origin,
    service::sync_runtime::marker_operation operation =
        service::sync_runtime::marker_operation::sync) {
    const auto zone_rows = co_await transaction.query(
        "SELECT revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenant_id, zone_id);
    if (zone_rows.empty()) {
        co_return std::nullopt;
    }
    const auto cluster_rows = co_await transaction.query(
        "SELECT hostname_prefix FROM sys_cluster WHERE tenant_id = $1 AND dns_zone_id = $2 AND "
        "status = 'enabled' AND deleted_at IS NULL",
        tenant_id, zone_id);
    if (cluster_rows.empty()) {
        co_return std::nullopt;
    }

    std::unordered_set<std::string> hostnames;
    hostnames.reserve(cluster_rows.size());
    for (const auto& row : cluster_rows) {
        hostnames.emplace(row[0].value().value_or(""));
    }

    const auto config_rows = co_await transaction.query(
        "SELECT config::text FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenant_id,
        zone_id);
    const std::optional<zone_config_data> config =
        parse_stored(config_rows.front()[0].value().value_or("{}"));
    if (!config) {
        throw std::runtime_error("DNS 聚合配置损坏");
    }

    zone_config_output filtered;
    auto& filtered_records = filtered.ensure<"records">();
    bool changed = false;
    for (const auto& record : config->records_) {
        if (is_cluster_managed_record(record, hostnames)) {
            changed = true;
            continue;
        }
        auto& output = filtered_records.emplace_back();
        output.set<"id">(record.id_);
        output.set<"type">(record.type_);
        output.set<"name">(record.name_);
        output.set<"content">(record.content_);
        output.set<"ttl">(record.ttl_);
        output.set<"proxied">(record.proxied_);
        output.set<"line_code">(record.line_code_);
        if (record.priority_) {
            output.set<"priority">(*record.priority_);
        }
    }
    if (!changed) {
        co_return std::nullopt;
    }

    const auto config_json = ruvia::to_json(filtered);
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, revision = revision + 1, "
        "desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE tenant_id = $2 AND id = $3 AND revision = $4 AND "
        "deleted_at IS NULL RETURNING desired_revision",
        std::string_view(config_json), tenant_id, zone_id,
        zone_rows.front()[0].as<std::int64_t>().value_or(0));
    if (updated.empty()) {
        co_return std::nullopt;
    }
    co_return co_await enqueue_zone_revision(transaction, tenant_id, zone_id,
        updated.front()[0].as<std::int64_t>().value_or(1), origin,
        operation);
}

inline ruvia::task<std::string> mark_zone_dirty(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& zone_id, service::operation::work_origin& origin,
    service::sync_runtime::marker_operation operation =
        service::sync_runtime::marker_operation::sync) {
    if (const auto pruned =
            co_await prune_cluster_managed_records(transaction, tenant_id, zone_id, origin, operation)) {
        co_return *pruned;
    }
    const auto rows = co_await transaction.query(
        "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
        "'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL RETURNING desired_revision",
        tenant_id, zone_id);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
    co_return co_await enqueue_zone_revision(
        transaction, tenant_id, zone_id, rows.front()[0].as<std::int64_t>().value_or(1), origin, operation);
}

inline ruvia::task<void> mark_provider_zones_dirty(ruvia::db_transaction& transaction,
    const std::string& tenant_id,
    const std::string& provider_id, service::operation::work_origin& origin) {
    const auto rows = co_await transaction.query(
        "UPDATE sys_dns_zone SET desired_revision = desired_revision + 1, sync_status = "
        "'pending', last_error = NULL, updated_at = NOW() WHERE tenant_id = $1 AND "
        "provider_id = $2 AND deleted_at IS NULL RETURNING id, desired_revision",
        tenant_id, provider_id);
    for (const auto& row : rows) {
        co_await enqueue_zone_revision(transaction, tenant_id,
            std::string(row[0].value().value_or("")),
            row[1].as<std::int64_t>().value_or(1), origin);
    }
    co_return;
}

}  // namespace service::dns_sync
