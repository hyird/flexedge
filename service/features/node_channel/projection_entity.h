#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <memory_resource>
#include <string>

namespace service::node_channel {

using projection_node_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"agent_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 32}>,
    ruvia::db_column<"node_secret_hash", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"registration_status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"name", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 100}>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"node_spec_revision", std::int64_t>,
    ruvia::db_column<"node_spec_digest", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"config", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"desired_release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using projection_node_index = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>,
    ruvia::db_column<"tenant_id", std::pmr::string>>;

using projection_release_entity = ruvia::db_entity<"sys_cluster_release",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"manifest_digest", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"manifest_envelope", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text, .nullable_ = true}>>;

using projection_mapping_entity = ruvia::db_entity<"sys_cluster_release_object",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"position", std::int32_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"object_digest", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .length_ = 64}>>;

using projection_object_entity = ruvia::db_entity<"sys_delivery_object",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"digest_sha256", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .primary_key_ = true, .length_ = 64}>,
    ruvia::db_column<"kind", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"payload_envelope", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}>>;

}  // namespace service::node_channel
