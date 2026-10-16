#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace service::dns_zone {

// A live managed zone another resource may claim names under.
struct dns_zone_reference {
    std::string id_;
    std::string domain_;
};

// Locks every live zone of the tenant FOR SHARE, longest domain first, so a
// referencing write sees a stable zone set until it commits. Lock order: the
// caller has locked its own row and any cluster it references; certificates
// are locked after zones.
inline ruvia::task<std::vector<dns_zone_reference>> lock_live_dns_zones(ruvia::db_transaction& transaction,
    std::string_view tenant_id) {
    const auto rows = co_await transaction.query(
        "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL "
        "ORDER BY length(domain) DESC, sort ASC FOR SHARE",
        tenant_id);
    std::vector<dns_zone_reference> zones;
    zones.reserve(rows.size());
    for (const auto& row : rows) {
        zones.push_back({std::string(row[0].value().value()), std::string(row[1].value().value())});
    }
    co_return zones;
}

// One live zone locked by a resource that depends on it.
struct locked_dns_zone {
    std::string domain_;
    std::string provider_id_;
    bool synced_;
    std::string runtime_json_;
};

// Locks one live zone FOR SHARE; `std::nullopt` when it is missing or deleted.
// Lock order: the caller has locked its own row and any cluster or certificate
// provider it references; the zone's DNS provider is locked after the zone.
inline ruvia::task<std::optional<locked_dns_zone>> lock_dns_zone(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view zone_id) {
    const auto rows = co_await transaction.query(
        "SELECT domain, provider_id, sync_status = 'synced', runtime::text FROM sys_dns_zone WHERE "
        "id = $1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        zone_id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return locked_dns_zone{std::string(row[0].value().value_or("")), std::string(row[1].value().value_or("")),
        row[2].as<bool>().value_or(false), std::string(row[3].value().value_or("{}"))};
}

// Whether a zone still depends on the DNS provider: a live zone, or a deleted
// one whose remote cleanup has not synced. The caller holds the provider FOR
// UPDATE, so no zone can start referencing it meanwhile.
inline ruvia::task<bool> dns_zones_use_provider(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_dns_zone WHERE tenant_id = $1 AND provider_id = $2 AND (deleted_at IS NULL "
        "OR synced_revision < desired_revision OR sync_status <> 'synced') LIMIT 1",
        tenant_id, provider_id);
    co_return !rows.empty();
}

}  // namespace service::dns_zone
