#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <memory_resource>
#include <string>

namespace service::node_presence {

// Read-only projection of the node fields used when rebuilding Redis presence.
using node_presence_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string,
        ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string,
        ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"last_heartbeat_at", std::pmr::string,
        ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using seed_projection = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>,
    ruvia::db_column<"tenant_id", std::pmr::string>, ruvia::db_column<"deadline_unix_ms", std::int64_t>>;

}  // namespace service::node_presence
