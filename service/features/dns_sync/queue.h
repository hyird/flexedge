#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "service/common/database.h"
#include "service/common/domain_name.h"
#include "service/features/background/queued_work.h"
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

// Queues the sync task of a zone revision that is already stored: a new
// zone, or a current revision whose task was lost.
inline ruvia::task<std::string>
enqueue_zone_revision(ruvia::db_transaction& transaction, std::string_view tenant_id, std::string_view zone_id,
    std::int64_t revision, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND deleted_at IS NULL LIMIT 1", tenant_id,
        zone_id);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
    co_return co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
        service::sync_runtime::marker_resource_type::dns_zone, zone_id, service::sync_runtime::marker_operation::sync,
        revision, work);
}

// What one zone revision changes besides its desired revision.
struct zone_revision_change final {
    // Replaces the records configuration and advances the configuration revision.
    std::optional<std::string_view> config_json_{};
    // Replaces the runtime (lines, record states, conflicts, challenge records).
    std::optional<std::string_view> runtime_json_{};
    // Fences: the change applies only while the zone is at these revisions.
    std::optional<std::int64_t> expected_revision_{};
    std::optional<std::int64_t> expected_desired_revision_{};
    // The sync task's operation; `remove` also deletes the zone and advances
    // its configuration revision.
    service::sync_runtime::marker_operation operation_{service::sync_runtime::marker_operation::sync};
};

struct zone_revision final {
    std::int64_t revision_{};
    std::int64_t desired_revision_{};
    // The zone's sync task, queued at desired_revision_.
    std::string task_id_{};
};

// The one writer of a zone's desired revision: applies `change`, advances the
// desired revision, marks the zone pending and queues its sync task at the new
// revision. nullopt when the zone is deleted or a fence no longer matches.
inline ruvia::task<std::optional<zone_revision>> advance_zone_revision(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view zone_id, const zone_revision_change& change,
    service::background::queued_work& work) {
    const bool removal = change.operation_ == service::sync_runtime::marker_operation::remove;
    ruvia::db_query update;
    const auto increment = [&](std::string_view column) {
        return update.binary(update.column(column), ruvia::db_binary_operator::add, update.value(std::int64_t{1}));
    };
    const auto equals = [&](std::string_view column, ruvia::db_expression value) {
        return update.binary(update.column(column), ruvia::db_binary_operator::equal, value);
    };
    update.update("sys_dns_zone")
        .set("desired_revision", increment("desired_revision"))
        .set("sync_status", update.value(std::string_view{"pending"}))
        .set("last_error", update.null_value())
        .set("updated_at", update.call("now"));
    if (change.config_json_ || removal) {
        update.set("revision", increment("revision"));
    }
    if (change.config_json_) {
        update.set("config", update.cast(update.value(*change.config_json_), ruvia::db_data_type::jsonb));
    }
    if (change.runtime_json_) {
        update.set("runtime", update.cast(update.value(*change.runtime_json_), ruvia::db_data_type::jsonb));
    }
    if (removal) {
        update.set("deleted_at", update.call("now"));
    }
    update.where(equals("tenant_id", update.value(tenant_id)))
        .and_where(equals("id", update.value(zone_id)))
        .and_where(update.unary(ruvia::db_unary_operator::is_null, update.column("deleted_at")));
    if (change.expected_revision_) {
        update.and_where(equals("revision", update.value(*change.expected_revision_)));
    }
    if (change.expected_desired_revision_) {
        update.and_where(equals("desired_revision", update.value(*change.expected_desired_revision_)));
    }
    update.returning({update.column("revision"), update.column("desired_revision")});
    const auto rows = co_await transaction.query(update);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    zone_revision result{.revision_ = rows.front()[0].as<std::int64_t>().value_or(1),
        .desired_revision_ = rows.front()[1].as<std::int64_t>().value_or(1)};
    result.task_id_ = co_await service::sync_runtime::upsert_marker(transaction, tenant_id,
        service::sync_runtime::marker_resource_type::dns_zone, zone_id, change.operation_, result.desired_revision_,
        work);
    co_return result;
}

// Drops configured records that collide with the A/AAAA/CNAME records the
// zone's clusters project. True when the configuration changed (and a new
// revision was queued).
inline ruvia::task<bool> prune_cluster_managed_records(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view zone_id, service::background::queued_work& work) {
    const auto zone_rows = co_await transaction.query(
        "SELECT revision, config::text FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenant_id, zone_id);
    if (zone_rows.empty()) {
        co_return false;
    }
    const auto cluster_rows = co_await transaction.query(
        "SELECT hostname_prefix FROM sys_cluster WHERE tenant_id = $1 AND dns_zone_id = $2 AND "
        "status = 'enabled' AND deleted_at IS NULL",
        tenant_id, zone_id);
    if (cluster_rows.empty()) {
        co_return false;
    }

    std::unordered_set<std::string> hostnames;
    hostnames.reserve(cluster_rows.size());
    for (const auto& row : cluster_rows) {
        hostnames.emplace(row[0].value().value_or(""));
    }

    const std::optional<zone_config_data> config = parse_stored(zone_rows.front()[1].value().value_or("{}"));
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
        co_return false;
    }

    const auto config_json = ruvia::to_json(filtered);
    co_return (co_await advance_zone_revision(transaction, tenant_id, zone_id,
                   {.config_json_ = std::string_view(config_json),
                       .expected_revision_ = zone_rows.front()[0].as<std::int64_t>().value_or(0)},
                   work))
        .has_value();
}

// The records a zone projects changed (a cluster's nodes, a website's
// domains): queues a new revision.
inline ruvia::task<void> mark_zone_dirty(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view zone_id, service::background::queued_work& work) {
    if (co_await prune_cluster_managed_records(transaction, tenant_id, zone_id, work)) {
        co_return;
    }
    if (!co_await advance_zone_revision(transaction, tenant_id, zone_id, {}, work)) {
        throw std::runtime_error("DNS 聚合根不存在");
    }
}

// The A/AAAA records a cluster projects into its access zone changed.
inline ruvia::task<void> mark_cluster_zone_dirty(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view cluster_id, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT dns_zone_id FROM sys_cluster WHERE tenant_id = $1 AND id = $2 LIMIT 1", tenant_id, cluster_id);
    if (!rows.empty()) {
        co_await mark_zone_dirty(transaction, tenant_id, rows.front()[0].value().value_or(""), work);
    }
}

// The credentials of a DNS provider changed: every zone it hosts resyncs.
inline ruvia::task<void> mark_provider_zones_dirty(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND provider_id = $2 AND deleted_at IS NULL ORDER BY "
        "id FOR UPDATE",
        tenant_id, provider_id);
    for (const auto& row : rows) {
        (void)co_await advance_zone_revision(transaction, tenant_id, row[0].value().value_or(""), {}, work);
    }
}

// One DNS-01 TXT record a certificate keeps in a zone.
struct challenge_record_spec final {
    std::string name_;
    std::string content_;
};

// Replaces the challenge records `certificate_id` keeps in the zone; no
// records retires them. nullopt when the zone is deleted or nothing changed.
inline ruvia::task<std::optional<zone_revision>> replace_challenge_records(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view zone_id, std::string_view certificate_id,
    std::span<const challenge_record_spec> records, std::int64_t ttl, service::background::queued_work& work) {
    const auto rows = co_await transaction.query(
        "SELECT runtime::text, desired_revision FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
        "deleted_at IS NULL LIMIT 1 FOR UPDATE",
        tenant_id, zone_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    auto runtime = parse_stored_runtime(rows.front()[0].value().value_or("{}"));
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    const auto removed = std::erase_if(runtime->challenge_records_,
        [&](const auto& challenge) { return challenge.certificate_id_ == certificate_id; });
    if (removed == 0 && records.empty()) {
        co_return std::nullopt;
    }
    if (!records.empty()) {
        const auto ids = co_await transaction.query(
            "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)",
            static_cast<std::int64_t>(records.size()));
        if (ids.size() != records.size()) {
            throw std::runtime_error("DNS-01 Challenge ID 生成失败");
        }
        for (std::size_t index = 0; index < records.size(); ++index) {
            runtime->challenge_records_.push_back({
                .id_ = std::string(ids[index][0].value().value_or("")),
                .name_ = records[index].name_,
                .content_ = records[index].content_,
                .ttl_ = ttl,
                .certificate_id_ = std::string(certificate_id),
            });
        }
    }
    const auto runtime_json = ruvia::to_json(to_output(*runtime));
    co_return co_await advance_zone_revision(transaction, tenant_id, zone_id,
        {.runtime_json_ = std::string_view(runtime_json),
            .expected_desired_revision_ = rows.front()[1].as<std::int64_t>().value_or(0)},
        work);
}

}  // namespace service::dns_sync
