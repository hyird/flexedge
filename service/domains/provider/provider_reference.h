#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/dns/provider_runtime.h"

namespace service::provider {

// A live DNS provider account (`kind = 'dns'`, not deleted) over alias
// `table`: the provider rows other domains may join to.
inline ruvia::db_expression live_dns_provider_predicate(ruvia::db_query& query, std::string_view table) {
    return query.binary(
        query.binary(query.column("kind", table), ruvia::db_binary_operator::equal, query.value("dns")),
        ruvia::db_binary_operator::and_value,
        query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at", table)));
}

// Zones last reported by a verified live DNS provider; `std::nullopt` when the
// provider is missing, deleted, not a DNS provider or not verified.
template <typename database_type>
ruvia::task<std::optional<std::vector<service::dns::dns_provider_zone_runtime_data>>> verified_dns_provider_zones(
    database_type& database, std::string_view tenant_id, std::string_view provider_id,
    std::pmr::memory_resource* resource) {
    const auto rows = co_await database.query(
        "SELECT runtime::text FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind "
        "= 'dns' AND status = 'verified' AND deleted_at IS NULL LIMIT 1",
        provider_id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return service::dns::parse_dns_provider_runtime(rows.front()[0].value().value_or("{}"), resource);
}

// A live DNS provider account another resource depends on.
struct dns_provider_reference {
    std::string provider_;
    bool verified_;
};

// Locks a live DNS provider FOR SHARE; `std::nullopt` when it is missing,
// deleted or not a DNS provider. Lock order: the caller has locked its own row
// and the zone served by the provider; nothing is locked after it.
inline ruvia::task<std::optional<dns_provider_reference>> lock_dns_provider(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view provider_id) {
    const auto rows = co_await transaction.query(
        "SELECT provider, status = 'verified' FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND "
        "kind = 'dns' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        provider_id, tenant_id);
    if (rows.empty()) {
        co_return std::nullopt;
    }
    co_return dns_provider_reference{std::string(rows.front()[0].value().value_or("")),
        rows.front()[1].as<bool>().value_or(false)};
}

// Locks a verified live certificate provider FOR SHARE; false when it is
// missing, deleted, unverified or not a certificate provider. Lock order: the
// caller has locked its own row; the zone and its DNS provider are locked after.
inline ruvia::task<bool> lock_verified_certificate_provider(ruvia::db_transaction& transaction,
    std::string_view tenant_id, std::string_view provider_id) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = 'certificate' AND "
        "status = 'verified' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        provider_id, tenant_id);
    co_return !rows.empty();
}

}  // namespace service::provider
