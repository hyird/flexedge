#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <string_view>
#include <vector>

#include "service/features/website_config/mapper.h"

namespace service::website {

// Cache/security policy publication and website binding mutate the same website
// aggregates. Serialize them before taking any website or cluster row locks.
inline ruvia::task<void> lock_policy_bindings(ruvia::db_transaction& db, const std::string& tenant) {
    co_await db.query("SELECT pg_advisory_xact_lock(hashtextextended($1,0))", "website-policy-bindings:" + tenant);
}

// Whether a website claims a name under the zone. The caller holds the zone
// FOR UPDATE, so no website can start claiming under it meanwhile.
inline ruvia::task<bool> websites_claim_dns_zone(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view zone_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_website_domain_claim WHERE tenant_id = $1 AND dns_zone_id = $2 LIMIT 1",
        tenant_id, zone_id);
    co_return !rows.empty();
}

// Whether a website serves the certificate. The caller holds the certificate
// FOR UPDATE, so no website can start binding it meanwhile.
inline ruvia::task<bool> websites_bind_certificate(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view certificate_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_website_certificate_binding WHERE tenant_id = $1 AND certificate_id = $2 LIMIT 1",
        tenant_id, certificate_id);
    co_return !rows.empty();
}

// How the websites of a cluster still depend on it.
struct cluster_website_usage {
    bool live_;
    bool cleanup_active_;
};

// Live websites of the cluster, and deleted ones whose delete task has not
// finished. The caller holds the cluster FOR UPDATE, so no website can move
// onto it meanwhile.
inline ruvia::task<cluster_website_usage> websites_use_cluster(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT EXISTS(SELECT 1 FROM sys_website WHERE tenant_id = $1 AND cluster_id = $2 AND "
        "deleted_at IS NULL), EXISTS(SELECT 1 FROM sys_website website INNER JOIN sys_sync_task task ON "
        "task.tenant_id = website.tenant_id AND task.resource_type = 'website' AND task.resource_id = "
        "website.id AND task.operation = 'delete' AND NOT task.is_done WHERE website.tenant_id = $1 AND "
        "website.cluster_id = $2 AND website.deleted_at IS NOT NULL)",
        tenant_id, cluster_id);
    co_return cluster_website_usage{rows.front()[0].as<bool>().value_or(false),
        rows.front()[1].as<bool>().value_or(false)};
}

struct cache_website_reference final {
    std::string id_{};
    std::string cluster_id_{};
    std::string status_{};
    std::int64_t revision_{};
    service::website_config::website_config_data config_{};
};
inline ruvia::task<std::vector<cache_website_reference>> cache_website_references(auto& db, const std::string& tenant, const std::string& policy_id = {}) {
    const auto rows = co_await db.query("SELECT id,cluster_id,status,revision,config::text FROM sys_website WHERE tenant_id=$1 AND deleted_at IS NULL AND ($2='' OR cache_policy_id=NULLIF($2,'')::uuid) ORDER BY sort DESC", tenant, policy_id);
    std::vector<cache_website_reference> result;
    for (const auto& row : rows) {
        auto config = service::website_config::parse_stored(row[4].value().value());
        if (!config) {
            throw std::runtime_error("invalid website configuration");
        }
        result.push_back({.id_ = std::string(row[0].value().value()), .cluster_id_ = std::string(row[1].value().value()), .status_ = std::string(row[2].value().value()), .revision_ = row[3].template as<std::int64_t>().value(), .config_ = std::move(*config)});
    }
    co_return result;
}

inline ruvia::task<std::int64_t> cache_policy_reference_count(auto& db, const std::string& tenant, const std::string& id) {
    const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_website WHERE tenant_id=$1 AND cache_policy_id=$2 AND deleted_at IS NULL", tenant, id);
    co_return rows.front()[0].template as<std::int64_t>().value();
}
struct cache_policy_publication final {
    std::string id_{};
    std::string cluster_id_{};
    std::int64_t revision_{};
};
inline ruvia::task<std::vector<cache_policy_publication>> revise_cache_policy_websites(
    ruvia::db_transaction& tx, const std::string& tenant, const std::string& id) {
    const auto rows = co_await tx.query("UPDATE sys_website SET revision=revision+1,updated_at=NOW() WHERE tenant_id=$1 AND cache_policy_id=$2 AND deleted_at IS NULL RETURNING id,cluster_id,revision", tenant, id);
    std::vector<cache_policy_publication> result;
    for (const auto& row : rows) {
        result.push_back({std::string(row[0].value().value()), std::string(row[1].value().value()), row[2].as<std::int64_t>().value()});
    }
    co_return result;
}
inline ruvia::task<std::int64_t> security_policy_reference_count(auto& db, const std::string& tenant, const std::string& id) {
    const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_website WHERE tenant_id=$1 AND security_policy_id=$2 AND deleted_at IS NULL", tenant, id);
    co_return rows.front()[0].template as<std::int64_t>().value();
}
struct security_policy_publication final {
    std::string id_{}, cluster_id_{};
    std::int64_t revision_{};
};
inline ruvia::task<std::vector<security_policy_publication>> revise_security_policy_websites(
    ruvia::db_transaction& tx, const std::string& tenant, const std::string& id) {
    const auto rows = co_await tx.query("UPDATE sys_website SET revision=revision+1,updated_at=NOW() WHERE tenant_id=$1 AND security_policy_id=$2 AND deleted_at IS NULL RETURNING id,cluster_id,revision", tenant, id);
    std::vector<security_policy_publication> result;
    for (const auto& row : rows) {
        result.push_back({std::string(row[0].value().value()), std::string(row[1].value().value()), row[2].as<std::int64_t>().value()});
    }
    co_return result;
}
inline ruvia::task<std::optional<cache_website_reference>> lock_cache_website(
    ruvia::db_transaction& tx, const std::string& tenant, const std::string& id) {
    const auto rows = co_await tx.query("SELECT cluster_id,config::text,status,revision FROM sys_website WHERE tenant_id=$1 AND id=$2 AND deleted_at IS NULL FOR UPDATE", tenant, id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    const auto config = service::website_config::parse_stored(row[1].value().value());
    if (!config) {
        throw std::runtime_error("invalid website configuration");
    }
    co_return cache_website_reference{.id_ = id, .cluster_id_ = std::string(row[0].value().value()), .status_ = std::string(row[2].value().value()), .revision_ = row[3].as<std::int64_t>().value(), .config_ = *config};
}
}  // namespace service::website
