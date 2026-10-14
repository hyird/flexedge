#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_transaction.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/database.h"
#include "service/domains/website/domain_claim_types.h"

namespace service::website {
inline bool is_domain_claim_conflict(const ruvia::db_error& error) {
    return service::common::is_unique_constraint_violation(error, "uq_website_domain_claim");
}
inline ruvia::task<bool> has_conflicting_domain_claims(
    ruvia::db_transaction& transaction, const std::string& tenant_id,
    std::span<const domain_claim> claims,
    const std::optional<std::string>& excluded_website_id) {
    if (claims.empty()) {
        co_return false;
    }
    std::vector<ruvia::db_value> params{ruvia::db_value{tenant_id}};
    params.reserve(1 + claims.size() + (excluded_website_id ? 1 : 0));
    std::string placeholders;
    for (const auto& claim : claims) {
        if (!placeholders.empty()) {
            placeholders += ", ";
        }
        placeholders += "$" + std::to_string(params.size() + 1);
        params.emplace_back(std::string_view{claim.key_});
    }
    std::string sql =
        "SELECT 1 FROM sys_website_domain_claim WHERE tenant_id = $1 AND domain_key IN (" +
        placeholders + ")";
    if (excluded_website_id) {
        sql += " AND website_id <> $" + std::to_string(params.size() + 1);
        params.emplace_back(std::string_view{*excluded_website_id});
    }
    sql += " LIMIT 1";
    co_return !(co_await transaction.query(sql, params)).empty();
}

inline ruvia::task<void> clear_relation_projections(ruvia::db_transaction& transaction,
    const std::string& tenant_id, const std::string& website_id) {
    (void)co_await transaction.execute(
        "DELETE FROM sys_website_certificate_binding WHERE tenant_id = $1 AND website_id "
        "= $2",
        tenant_id, website_id);
    (void)co_await transaction.execute(
        "DELETE FROM sys_website_domain_claim WHERE tenant_id = $1 AND website_id = $2",
        tenant_id, website_id);
}

inline ruvia::task<void>
replace_relation_projections(ruvia::db_transaction& transaction, const std::string& tenant_id,
    const std::string& website_id,
    std::span<const domain_claim> domain_claims,
    std::span<const std::string> certificate_ids) {
    co_await clear_relation_projections(transaction, tenant_id, website_id);

    if (!domain_claims.empty()) {
        std::vector<ruvia::db_value> domain_insert_params;
        domain_insert_params.reserve(domain_claims.size() * 6);
        std::string domain_insert_sql =
            "INSERT INTO sys_website_domain_claim (tenant_id, website_id, domain_id, "
            "domain_key, dns_mode, dns_zone_id) VALUES ";
        const auto append_param = [&](ruvia::db_value value) {
            domain_insert_sql += "$" + std::to_string(domain_insert_params.size() + 1);
            domain_insert_params.push_back(std::move(value));
        };
        for (const auto& claim : domain_claims) {
            if (!domain_insert_params.empty()) {
                domain_insert_sql += ", ";
            }
            domain_insert_sql += "(";
            append_param(ruvia::db_value{tenant_id});
            domain_insert_sql += ", ";
            append_param(ruvia::db_value{website_id});
            domain_insert_sql += ", ";
            append_param(ruvia::db_value{std::string_view{claim.id_}});
            domain_insert_sql += ", ";
            append_param(ruvia::db_value{std::string_view{claim.key_}});
            domain_insert_sql += ", ";
            append_param(ruvia::db_value{std::string_view{claim.dns_mode_}});
            domain_insert_sql += ", ";
            append_param(claim.dns_zone_id_ ? ruvia::db_value{std::string_view{*claim.dns_zone_id_}}
                                            : ruvia::db_value{nullptr});
            domain_insert_sql += ")";
        }
        (void)co_await transaction.execute(domain_insert_sql, domain_insert_params);
    }

    if (!certificate_ids.empty()) {
        std::vector<ruvia::db_value> certificate_insert_params;
        certificate_insert_params.reserve(certificate_ids.size() * 4);
        std::string certificate_insert_sql =
            "INSERT INTO sys_website_certificate_binding (tenant_id, website_id, "
            "certificate_id, position) VALUES ";
        const auto append_param = [&](ruvia::db_value value) {
            certificate_insert_sql += "$" + std::to_string(certificate_insert_params.size() + 1);
            certificate_insert_params.push_back(std::move(value));
        };
        for (std::size_t position = 0; position < certificate_ids.size(); ++position) {
            if (position > 0) {
                certificate_insert_sql += ", ";
            }
            certificate_insert_sql += "(";
            append_param(ruvia::db_value{tenant_id});
            certificate_insert_sql += ", ";
            append_param(ruvia::db_value{website_id});
            certificate_insert_sql += ", ";
            append_param(ruvia::db_value{certificate_ids[position]});
            certificate_insert_sql += ", ";
            append_param(ruvia::db_value{static_cast<std::int64_t>(position)});
            certificate_insert_sql += ")";
        }
        (void)co_await transaction.execute(certificate_insert_sql, certificate_insert_params);
    }
    co_return;
}

}  // namespace service::website
