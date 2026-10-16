#pragma once

#include <ruvia/web/db/db_query.h>

#include <string_view>

namespace service::dns_zone {

// Zones needing attention: shared by the overview `dns_zone_issue_count` and
// the list filter `sync_status=issue`, so both always use one definition.
inline ruvia::db_expression dns_zone_issue_predicate(ruvia::db_query& query, std::string_view table) {
    return query.binary(query.column("sync_status", table), ruvia::db_binary_operator::in,
        query.list({query.value("failed"), query.value("conflict")}));
}

// `sync_status=issue`: the list filter for this predicate; other values match one
// stored status (ck_dns_zone_sync_status).
inline constexpr std::string_view dns_zone_issue_filter{"issue"};

}  // namespace service::dns_zone
