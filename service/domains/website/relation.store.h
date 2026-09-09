#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <ruvia/core/Task.h>
#include <ruvia/web/db/DbTransaction.h>
#include "service/common/database.h"
#include "service/domains/website/domain_claim.types.h"

namespace service::website {
inline bool isDomainClaimConflict(const ruvia::DbError& error) {
    return service::common::isUniqueConstraintViolation(error, "uq_website_domain_claim");
}
inline ruvia::Task<bool> hasConflictingDomainClaims(
    ruvia::DbTransaction& transaction, const std::string& tenantId,
    std::span<const DomainClaim> claims,
    const std::optional<std::string>& excludedWebsiteId) {
    if (claims.empty()) co_return false;
    std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
    params.reserve(1 + claims.size() + (excludedWebsiteId ? 1 : 0));
    std::string placeholders;
    for (const auto& claim : claims) {
        if (!placeholders.empty()) placeholders += ", ";
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(std::string_view{claim.key});
    }
    std::string sql =
        "SELECT 1 FROM sys_website_domain_claim WHERE tenant_id = $1 AND domain_key IN (" +
        placeholders + ")";
    if (excludedWebsiteId) {
        sql += " AND website_id <> $" + std::to_string(params.size() + 1);
        params.emplace_back(std::string_view{*excludedWebsiteId});
    }
    sql += " LIMIT 1";
    co_return !(co_await transaction.query(sql, params)).empty();
}

inline ruvia::Task<void> clearRelationProjections(ruvia::DbTransaction& transaction,
    const std::string& tenantId, const std::string& websiteId) {
    (void)co_await transaction.execute(
        "DELETE FROM sys_website_certificate_binding WHERE tenant_id = $1 AND website_id "
        "= $2",
        tenantId, websiteId);
    (void)co_await transaction.execute(
        "DELETE FROM sys_website_domain_claim WHERE tenant_id = $1 AND website_id = $2",
        tenantId, websiteId);

}

inline ruvia::Task<void>
replaceRelationProjections(ruvia::DbTransaction& transaction, const std::string& tenantId,
                           const std::string& websiteId,
                           std::span<const DomainClaim> domainClaims,
                           std::span<const std::string> certificateIds) {
    co_await clearRelationProjections(transaction, tenantId, websiteId);

    if (!domainClaims.empty()) {
        std::vector<ruvia::DbValue> domainInsertParams;
        domainInsertParams.reserve(domainClaims.size() * 6);
        std::string domainInsertSql =
            "INSERT INTO sys_website_domain_claim (tenant_id, website_id, domain_id, "
            "domain_key, dns_mode, dns_zone_id) VALUES ";
        const auto appendParam = [&](ruvia::DbValue value) {
            domainInsertSql += "$" + std::to_string(domainInsertParams.size() + 1);
            domainInsertParams.push_back(std::move(value));
        };
        for (const auto& claim : domainClaims) {
            if (!domainInsertParams.empty()) {
                domainInsertSql += ", ";
            }
            domainInsertSql += "(";
            appendParam(ruvia::DbValue{tenantId});
            domainInsertSql += ", ";
            appendParam(ruvia::DbValue{websiteId});
            domainInsertSql += ", ";
            appendParam(ruvia::DbValue{std::string_view{claim.id}});
            domainInsertSql += ", ";
            appendParam(ruvia::DbValue{std::string_view{claim.key}});
            domainInsertSql += ", ";
            appendParam(ruvia::DbValue{std::string_view{claim.dnsMode}});
            domainInsertSql += ", ";
            appendParam(claim.dnsZoneId ? ruvia::DbValue{std::string_view{*claim.dnsZoneId}}
                                        : ruvia::DbValue{nullptr});
            domainInsertSql += ")";
        }
        (void)co_await transaction.execute(domainInsertSql, domainInsertParams);
    }

    if (!certificateIds.empty()) {
        std::vector<ruvia::DbValue> certificateInsertParams;
        certificateInsertParams.reserve(certificateIds.size() * 4);
        std::string certificateInsertSql =
            "INSERT INTO sys_website_certificate_binding (tenant_id, website_id, "
            "certificate_id, position) VALUES ";
        const auto appendParam = [&](ruvia::DbValue value) {
            certificateInsertSql += "$" + std::to_string(certificateInsertParams.size() + 1);
            certificateInsertParams.push_back(std::move(value));
        };
        for (std::size_t position = 0; position < certificateIds.size(); ++position) {
            if (position > 0) {
                certificateInsertSql += ", ";
            }
            certificateInsertSql += "(";
            appendParam(ruvia::DbValue{tenantId});
            certificateInsertSql += ", ";
            appendParam(ruvia::DbValue{websiteId});
            certificateInsertSql += ", ";
            appendParam(ruvia::DbValue{certificateIds[position]});
            certificateInsertSql += ", ";
            appendParam(ruvia::DbValue{static_cast<std::int64_t>(position)});
            certificateInsertSql += ")";
        }
        (void)co_await transaction.execute(certificateInsertSql, certificateInsertParams);
    }
    co_return;
}

} // namespace service::website
