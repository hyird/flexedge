#pragma once

#include <ruvia/web/db/db_query.h>

#include <optional>
#include <string>
#include <string_view>

#include "service/common/http.h"

namespace service::certificate {

// Attention states shared by the overview counts and the certificate list
// filters (`expiring=1`, `abnormal=1`), so both always use one definition.

// A present attention filter must be exactly `1`; absence means no filter.
inline bool parse_attention_filter(std::optional<std::string_view> value, std::string_view name) {
    if (!value) {
        return false;
    }
    if (*value != "1") {
        service::common::throw_app_error(service::common::validation_error_code,
            std::string(name) + " 只能为 1", 400);
    }
    return true;
}

// Valid and expiring within 30 days (`certificate_expiring_count`, `expiring=1`).
inline ruvia::db_expression certificate_expiring_predicate(ruvia::db_query& query,
    std::string_view table) {
    const auto expires_at = query.column("expires_at", table);
    const auto now = query.call("now");
    return query.binary(
        query.binary(query.binary(query.column("status", table), ruvia::db_binary_operator::equal,
                         query.value("valid")),
            ruvia::db_binary_operator::and_value,
            query.binary(expires_at, ruvia::db_binary_operator::greater, now)),
        ruvia::db_binary_operator::and_value,
        query.binary(expires_at, ruvia::db_binary_operator::less_equal,
            query.binary(now, ruvia::db_binary_operator::add,
                query.cast(query.value("30 days"),
                    ruvia::db_data_type::interval))));
}

// Failed or expired, or valid with a recorded error (`certificate_failed_count`, `abnormal=1`).
inline ruvia::db_expression certificate_abnormal_predicate(ruvia::db_query& query,
    std::string_view table) {
    const auto status = query.column("status", table);
    return query.binary(
        query.binary(status, ruvia::db_binary_operator::in,
            query.list({query.value("failed"), query.value("expired")})),
        ruvia::db_binary_operator::or_value,
        query.binary(query.binary(status, ruvia::db_binary_operator::equal, query.value("valid")),
            ruvia::db_binary_operator::and_value,
            query.unary(ruvia::db_unary_operator::is_not_null,
                query.column("last_error", table))));
}

}  // namespace service::certificate
