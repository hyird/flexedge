#pragma once

#include <ruvia/web/db/db_query.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

namespace service::node {

// The control plane dictates the agent heartbeat cadence in Welcome and every HeartbeatAck;
// deployed agents follow it without an update.
inline constexpr std::chrono::seconds agent_heartbeat_interval{5};
// A node is online when registered, enabled and heard from within three heartbeat intervals:
// the rule of node lists, cluster and overview counts, and Redis presence deadlines.
inline constexpr std::chrono::seconds online_window{3 * agent_heartbeat_interval};

// last_heartbeat_at within the online window. Raw SQL states the same rule as
// `last_heartbeat_at >= NOW() - make_interval(secs => $n)` with $n bound to
// online_window_seconds.
inline constexpr std::int64_t online_window_seconds{online_window.count()};

inline ruvia::db_expression heartbeat_within_online_window(ruvia::db_query& query,
    std::string_view table) {
    const std::array seconds{ruvia::db_named_argument{.name_ = "secs",
        .value_ = query.value(online_window_seconds)}};
    return query.binary(query.column("last_heartbeat_at", table),
        ruvia::db_binary_operator::greater_equal,
        query.binary(query.call("now"), ruvia::db_binary_operator::subtract,
            query.call("make_interval", {}, seconds)));
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

}  // namespace service::node
