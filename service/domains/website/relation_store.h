#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/db/db_query.h>
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
    ruvia::db_query query;
    std::vector<ruvia::db_expression> keys;
    keys.reserve(claims.size());
    for (const auto& claim : claims) {
        keys.push_back(query.value(claim.key_));
    }
    query.select(query.sql("1"))
        .from("sys_website_domain_claim")
        .where(query.sql({"tenant_id = ", ""}, {query.value(tenant_id)}))
        .and_where(query.binary(query.sql("domain_key"), ruvia::db_binary_operator::in, query.list(keys)));
    if (excluded_website_id) {
        query.and_where(query.sql({"website_id <> ", ""}, {query.value(*excluded_website_id)}));
    }
    query.limit(1);
    co_return !(co_await transaction.query(query)).empty();
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
        ruvia::db_query insert;
        insert.insert_into("sys_website_domain_claim",
            {"tenant_id", "website_id", "domain_id", "domain_key", "dns_mode", "dns_zone_id"});
        for (const auto& claim : domain_claims) {
            insert.values({insert.value(tenant_id), insert.value(website_id), insert.value(claim.id_),
                insert.value(claim.key_), insert.value(claim.dns_mode_),
                claim.dns_zone_id_ ? insert.value(*claim.dns_zone_id_) : insert.null_value()});
        }
        (void)co_await transaction.execute(insert);
    }

    if (!certificate_ids.empty()) {
        ruvia::db_query insert;
        insert.insert_into("sys_website_certificate_binding",
            {"tenant_id", "website_id", "certificate_id", "position"});
        for (std::size_t position = 0; position < certificate_ids.size(); ++position) {
            insert.values({insert.value(tenant_id), insert.value(website_id),
                insert.value(certificate_ids[position]),
                insert.value(static_cast<std::int64_t>(position))});
        }
        (void)co_await transaction.execute(insert);
    }
    co_return;
}

}  // namespace service::website
