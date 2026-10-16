#pragma once

#include <ruvia/web/db/db_entity.h>

#include <cstdint>
#include <memory_resource>
#include <string>

namespace service::live_resource {

using outbox_entity = ruvia::db_entity<"sys_notification_outbox",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .primary_key_ = true, .length_ = 64}>,
    ruvia::db_column<"kind", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 24}>,
    ruvia::db_column<"payload", std::pmr::string>,
    ruvia::db_column<"created_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"CURRENT_TIMESTAMP"}}>,
    ruvia::db_column<"available_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"CURRENT_TIMESTAMP"}}>,
    ruvia::db_column<"attempts", std::int32_t, ruvia::db_column_options{.default_expression_ = ruvia::fixed_string{"0"}}>>;

}  // namespace service::live_resource
