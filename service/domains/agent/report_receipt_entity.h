#pragma once

#include <ruvia/web/db/db_entity.h>

#include <memory_resource>
#include <string>

namespace service::agent {

using report_node_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"agent_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 32}>,
    ruvia::db_column<"node_secret_hash", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"registration_status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using report_receipt_entity = ruvia::db_entity<"sys_node_report_receipt",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"node_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"agent_id", std::pmr::string>,
    ruvia::db_column<"received_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"completed_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .default_expression_ = ruvia::fixed_string{"CURRENT_TIMESTAMP"}}>>;

}  // namespace service::agent
