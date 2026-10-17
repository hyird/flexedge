#pragma once
#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <string>
namespace service::cache {
using policy_entity = ruvia::db_entity<"sys_cache_policy",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"name", std::pmr::string>,
    ruvia::db_column<"description", std::pmr::string>,
    ruvia::db_column<"status", std::pmr::string>,
    ruvia::db_column<"revision", std::int64_t>,
    ruvia::db_column<"config", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"created_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"updated_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"deleted_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"sort", std::int64_t>>;
}
