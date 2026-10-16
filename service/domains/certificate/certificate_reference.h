#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace service::certificate {

// Configured names of a usable certificate (also known before issuance).
struct certificate_reference {
    std::string id_;
    std::vector<std::string> names_;
};

// Locks the selected certificates FOR SHARE when every one of them is live,
// issued and unexpired; `std::nullopt` when any is not. Lock order: the caller
// has locked its own row and the cluster and zones it references.
inline ruvia::task<std::optional<std::vector<certificate_reference>>> lock_usable_certificates(
    ruvia::db_transaction& transaction, std::string_view tenant_id, std::span<const std::string> certificate_ids) {
    std::vector<certificate_reference> certificates;
    if (certificate_ids.empty()) {
        co_return certificates;
    }
    ruvia::db_query query(std::pmr::new_delete_resource());
    const auto names = query.column("subject_alt_names");
    std::vector<ruvia::db_expression> ids;
    ids.reserve(certificate_ids.size());
    for (const auto& id : certificate_ids) {
        ids.push_back(query.value(id));
    }
    const auto all = [&](std::initializer_list<ruvia::db_expression> predicates) {
        auto result = *predicates.begin();
        for (auto it = predicates.begin() + 1; it != predicates.end(); ++it) {
            result = query.binary(result, ruvia::db_binary_operator::and_value, *it);
        }
        return result;
    };
    query.select({query.column("id"), query.subscript(names, query.value(std::int64_t{1})),
                     query.subscript(names, query.value(std::int64_t{2}))})
        .from("sys_certificate")
        .where(all({
            query.binary(query.column("tenant_id"), ruvia::db_binary_operator::equal, query.value(tenant_id)),
            query.binary(query.column("id"), ruvia::db_binary_operator::in, query.list(ids)),
            query.unary(ruvia::db_unary_operator::is_null, query.column("deleted_at")),
            query.binary(query.column("issued_revision"), ruvia::db_binary_operator::greater,
                query.value(std::int64_t{0})),
            query.binary(query.column("expires_at"), ruvia::db_binary_operator::greater, query.call("now")),
        }))
        .lock({.mode_ = ruvia::db_row_lock::share});
    const auto rows = co_await transaction.query(query);
    if (rows.size() != certificate_ids.size()) {
        co_return std::nullopt;
    }
    certificates.reserve(rows.size());
    for (const auto& row : rows) {
        auto& certificate = certificates.emplace_back();
        certificate.id_ = std::string(row[0].value().value());
        for (const auto index : {1U, 2U}) {
            if (const auto name = row[index].value(); name && !name->empty()) {
                certificate.names_.emplace_back(*name);
            }
        }
    }
    co_return certificates;
}

// Whether a live certificate is issued through the zone. The caller holds the
// zone FOR UPDATE, so no certificate can start referencing it meanwhile.
inline ruvia::task<bool> certificates_use_dns_zone(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view zone_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_certificate WHERE tenant_id = $1 AND dns_zone_id = $2 AND deleted_at IS NULL "
        "LIMIT 1",
        tenant_id, zone_id);
    co_return !rows.empty();
}

// Whether a live certificate is issued by the certificate provider. The caller
// holds the provider FOR UPDATE, so no certificate can start referencing it.
inline ruvia::task<bool> certificates_use_provider(ruvia::db_transaction& transaction, std::string_view tenant_id,
    std::string_view provider_id) {
    const auto rows = co_await transaction.query(
        "SELECT 1 FROM sys_certificate WHERE tenant_id = $1 AND provider_id = $2 AND deleted_at IS NULL "
        "LIMIT 1",
        tenant_id, provider_id);
    co_return !rows.empty();
}

}  // namespace service::certificate
