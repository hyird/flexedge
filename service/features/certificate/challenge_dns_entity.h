#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <string>

namespace service::certificate_issuance::worker_detail {

// Read mappings for the persisted DNS result an issuance depends on. These
// partial entities do not own or create the DNS and task tables.
using challenge_zone_entity = ruvia::db_entity<"sys_dns_zone",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"synced_revision", std::int64_t>,
    ruvia::db_column<"runtime", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>>;
using challenge_task_entity = ruvia::db_entity<"sys_sync_task",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"resource_type", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>,
    ruvia::db_column<"resource_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"version", std::int64_t>, ruvia::db_column<"is_done", bool>, ruvia::db_column<"is_ok", bool>>;
using challenge_dns_projection = ruvia::db_projection<ruvia::db_column<"synced_revision", std::int64_t>,
    ruvia::db_column<"runtime", std::pmr::string>,
    ruvia::db_column<"task_id", std::pmr::string, {.nullable_ = true}>,
    ruvia::db_column<"version", std::int64_t, {.nullable_ = true}>,
    ruvia::db_column<"is_done", bool, {.nullable_ = true}>,
    ruvia::db_column<"is_ok", bool, {.nullable_ = true}>>;

}  // namespace service::certificate_issuance::worker_detail
