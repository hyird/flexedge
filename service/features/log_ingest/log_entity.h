#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <memory_resource>
#include <string>

namespace service::log_ingest {

// The ingest transaction locks these ownership records before writing logs.
using log_node_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"agent_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 32}>,
    ruvia::db_column<"registration_status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using log_website_entity = ruvia::db_entity<"sys_website",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using access_log_entity = ruvia::db_entity<"sys_website_access_log",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"occurred_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .primary_key_ = true}>,
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true, .default_expression_ = ruvia::fixed_string{"gen_random_uuid()"}}>,
    ruvia::db_column<"node_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"website_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"client_ip", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"protocol", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"method", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>,
    ruvia::db_column<"host", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 253}>,
    ruvia::db_column<"target", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 2048}>,
    ruvia::db_column<"status_code", std::int32_t>,
    ruvia::db_column<"request_bytes", std::int64_t, ruvia::db_column_options{.default_expression_ = ruvia::fixed_string{"0"}}>,
    ruvia::db_column<"response_bytes", std::int64_t, ruvia::db_column_options{.default_expression_ = ruvia::fixed_string{"0"}}>,
    ruvia::db_column<"duration_ms", std::int32_t, ruvia::db_column_options{.default_expression_ = ruvia::fixed_string{"0"}}>,
    ruvia::db_column<"user_agent", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 512}>,
    ruvia::db_column<"referer", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 512}>,
    ruvia::db_column<"request_headers", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"request_body", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"request_body_truncated", bool, ruvia::db_column_options{.default_expression_ = ruvia::fixed_string{"false"}}>,
    ruvia::db_column<"tls_fingerprint", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 128}>,
    ruvia::db_column<"response_headers", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"query_string", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"cookies", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"created_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"CURRENT_TIMESTAMP"}}>>;

using node_log_entity = ruvia::db_entity<"sys_node_log",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"occurred_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .primary_key_ = true}>,
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true, .default_expression_ = ruvia::fixed_string{"gen_random_uuid()"}}>,
    ruvia::db_column<"node_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"level", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"category", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 64}>,
    ruvia::db_column<"message", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 2000}>,
    ruvia::db_column<"created_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"CURRENT_TIMESTAMP"}}>>;

using inserted_access_log = ruvia::db_projection<ruvia::db_column<"website_id", std::pmr::string>>;
using stored_log_identity = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>>;

}  // namespace service::log_ingest
