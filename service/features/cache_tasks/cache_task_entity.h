#pragma once
#include <ruvia/web/db/db_entity.h>

#include <cstdint>
#include <string>
namespace service::cache_tasks {
using task_entity = ruvia::db_entity<"sys_cache_task",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"object_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"website_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"cache_policy_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"operation", std::pmr::string>,
    ruvia::db_column<"scope", std::pmr::string>,
    ruvia::db_column<"payload", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"generation", std::int64_t>,
    ruvia::db_column<"status", std::pmr::string>,
    ruvia::db_column<"lease_owner", std::pmr::string, {.nullable_ = true}>,
    ruvia::db_column<"lease_until", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"next_attempt_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"attempt_count", std::int64_t>,
    ruvia::db_column<"failure_count", std::int64_t>,
    ruvia::db_column<"error", std::pmr::string>,
    ruvia::db_column<"started_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"last_success_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"updated_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>>;
}
