#pragma once

#include <ruvia/web/db/db_query.h>

#include <optional>
#include <string>
#include <string_view>

#include "service/common/database.h"
#include "service/common/http.h"

namespace service::dns_zone {

// Zones needing attention: shared by the overview `dns_zone_issue_count` and
// the list filter `sync_status=issue`, so both always use one definition.
inline ruvia::db_expression dns_zone_issue_predicate(ruvia::db_query& query, std::string_view table) {
    return query.binary(query.column("sync_status", table), ruvia::db_binary_operator::in,
        query.list({query.value("failed"), query.value("conflict")}));
}

// SQL text of the predicate over alias `zone`, for raw queries this round does
// not rewrite. Rendered once from the structured expression above.
inline const std::string& dns_zone_issue_sql() {
    static const std::string sql = service::common::render_constant_predicate(
        [](ruvia::db_query& query) { return dns_zone_issue_predicate(query, "zone"); });
    return sql;
}

inline constexpr std::string_view dns_zone_issue_filter{"issue"};

// `sync_status` list filter: `issue`, or one stored status (ck_dns_zone_sync_status).
inline std::optional<std::string> parse_sync_status_filter(std::optional<std::string_view> value) {
    if (!value) {
        return std::nullopt;
    }
    if (*value == dns_zone_issue_filter || *value == "pending" || *value == "synced" ||
        *value == "failed" || *value == "conflict") {
        return std::string(*value);
    }
    service::common::throw_app_error(service::common::validation_error_code, "sync_status 不正确", 400);
}

}  // namespace service::dns_zone
