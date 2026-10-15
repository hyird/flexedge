#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <optional>
#include <span>
#include <string>
#include <vector>

#include "service/domains/website/domain_claim_types.h"

namespace service::website {

inline ruvia::task<std::vector<dns_zone_reference>> lock_dns_zone_references(
    ruvia::db_transaction& transaction, const std::string& tenant_id) {
    const auto rows = co_await transaction.query(
        "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL "
        "ORDER BY length(domain) DESC, sort ASC FOR SHARE",
        tenant_id);
    std::vector<dns_zone_reference> zones;
    zones.reserve(rows.size());
    for (const auto& row : rows) {
        zones.push_back({std::string(row[0].value().value()),
            std::string(row[1].value().value())});
    }
    co_return zones;
}

inline ruvia::task<bool> lock_available_cluster(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& cluster_id) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND status = "
        "'enabled' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
        cluster_id, tenant_id);
    co_return !rows.empty();
}

// Locks the selected certificates; `std::nullopt` when any of them is unavailable.
inline ruvia::task<std::optional<std::vector<certificate_reference>>> lock_available_certificates(
    ruvia::db_transaction& transaction, const std::string& tenant_id,
    std::span<const std::string> certificate_ids) {
    std::vector<certificate_reference> certificates;
    if (certificate_ids.empty()) {
        co_return certificates;
    }
    std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
    params.reserve(1 + certificate_ids.size());
    std::string placeholders;
    for (const auto& certificate_id : certificate_ids) {
        if (!placeholders.empty()) {
            placeholders += ", ";
        }
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(certificate_id);
    }
    const auto rows = co_await transaction.query(
        "SELECT id, subject_alt_names[1], subject_alt_names[2] FROM sys_certificate WHERE "
        "tenant_id = $1 AND id IN (" +
            placeholders +
            ") AND deleted_at IS NULL AND issued_revision > 0 "
            "AND expires_at > NOW() FOR SHARE",
        params);
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

}  // namespace service::website
