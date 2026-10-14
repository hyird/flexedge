#pragma once

#include <ruvia/web/db/db_query.h>

#include <chrono>
#include <string>
#include <string_view>

#include "service/common/database.h"

namespace service::node {

// The control plane dictates the agent heartbeat cadence in Welcome and every HeartbeatAck;
// deployed agents follow it without an update.
inline constexpr std::chrono::seconds agent_heartbeat_interval{5};
// A node is online when registered, enabled and heard from within three heartbeat intervals:
// the rule of node lists, cluster and overview counts, and node_deadlines.
inline constexpr std::chrono::seconds online_window{3 * agent_heartbeat_interval};

// last_heartbeat_at within the online window.
inline ruvia::db_expression heartbeat_within_online_window(ruvia::db_query& query,
    std::string_view table) {
    return query.binary(query.column("last_heartbeat_at", table),
        ruvia::db_binary_operator::greater_equal,
        query.binary(query.call("now"), ruvia::db_binary_operator::subtract,
            query.cast(query.value(std::to_string(online_window.count()) + " seconds"),
                ruvia::db_data_type::interval)));
}

inline ruvia::db_expression node_online_predicate(ruvia::db_query& query, std::string_view table) {
    const auto equal = [&](std::string_view column, std::string_view value) {
        return query.binary(query.column(column, table), ruvia::db_binary_operator::equal,
            query.value(value));
    };
    return query.binary(
        query.binary(equal("registration_status", "registered"),
            ruvia::db_binary_operator::and_value, equal("status", "enabled")),
        ruvia::db_binary_operator::and_value, heartbeat_within_online_window(query, table));
}

// SQL text of the predicates over alias `node`, for raw queries this round does not
// rewrite. Rendered once from the structured expressions above.
inline const std::string& node_online_sql() {
    static const std::string sql = service::common::render_constant_predicate(
        [](ruvia::db_query& query) { return node_online_predicate(query, "node"); });
    return sql;
}

inline const std::string& node_heartbeat_recent_sql() {
    static const std::string sql = service::common::render_constant_predicate(
        [](ruvia::db_query& query) { return heartbeat_within_online_window(query, "node"); });
    return sql;
}

}  // namespace service::node
