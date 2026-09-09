#pragma once

#include <span>
#include <string>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/domains/website/domain_claim.types.h"

namespace service::website {

inline ruvia::Task<std::vector<DnsZoneReference>> lockDnsZoneReferences(
    ruvia::DbTransaction& transaction, const std::string& tenantId) {
    const auto rows = co_await transaction.query(
        "SELECT id, domain FROM sys_dns_zone WHERE tenant_id = $1 AND deleted_at IS NULL "
        "ORDER BY length(domain) DESC, sort ASC FOR SHARE", tenantId);
    std::vector<DnsZoneReference> zones;
    zones.reserve(rows.size());
    for (const auto& row : rows) {
        zones.push_back({std::string(row[0].value().value()),
                         std::string(row[1].value().value())});
    }
    co_return zones;
}

inline ruvia::Task<bool> lockAvailableCluster(ruvia::DbTransaction& transaction,
    const std::string& tenantId, const std::string& clusterId) {
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_cluster WHERE id = $1 AND tenant_id = $2 AND status = "
        "'enabled' AND deleted_at IS NULL LIMIT 1 FOR SHARE", clusterId, tenantId);
    co_return !rows.empty();
}

inline ruvia::Task<bool> lockAvailableCertificates(ruvia::DbTransaction& transaction,
    const std::string& tenantId, std::span<const std::string> certificateIds) {
    if (certificateIds.empty()) co_return true;
    std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
    params.reserve(1 + certificateIds.size());
    std::string placeholders;
    for (const auto& certificateId : certificateIds) {
        if (!placeholders.empty()) placeholders += ", ";
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(certificateId);
    }
    const auto rows = co_await transaction.query(
        "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND id IN (" +
        placeholders + ") AND deleted_at IS NULL AND issued_revision > 0 "
        "AND expires_at > NOW() FOR SHARE", params);
    co_return rows.size() == certificateIds.size();
}

} // namespace service::website
