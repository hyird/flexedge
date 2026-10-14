#pragma once

#include <ruvia/web/db/db_query.h>

#include <string_view>

namespace service::node {

// A node is online when registered, enabled and heard from within 90 seconds:
// the rule of node lists, cluster counts and node_deadlines (agent heartbeats
// arrive every 30 seconds).
inline ruvia::db_expression node_online_predicate(ruvia::db_query& query, std::string_view table) {
    const auto equal = [&](std::string_view column, std::string_view value) {
        return query.binary(query.column(column, table), ruvia::db_binary_operator::equal,
            query.value(value));
    };
    return query.binary(
        query.binary(equal("registration_status", "registered"),
            ruvia::db_binary_operator::and_value, equal("status", "enabled")),
        ruvia::db_binary_operator::and_value,
        query.binary(query.column("last_heartbeat_at", table),
            ruvia::db_binary_operator::greater_equal,
            query.binary(query.call("now"), ruvia::db_binary_operator::subtract,
                query.cast(query.value("90 seconds"),
                    ruvia::db_data_type::interval))));
}

}  // namespace service::node
