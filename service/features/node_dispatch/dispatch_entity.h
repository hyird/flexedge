#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

namespace service::node_dispatch {

// Only the columns read or written by publication are mapped here. Defaults
// belonging to the database remain unset on inserts.
using dispatch_cluster_entity = ruvia::db_entity<"sys_cluster",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"dns_zone_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"hostname_prefix", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 63}>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"release_generation", std::int64_t>,
    ruvia::db_column<"current_release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"updated_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_zone_entity = ruvia::db_entity<"sys_dns_zone",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"domain", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 253}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_release_entity = ruvia::db_entity<"sys_cluster_release",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"generation", std::int64_t>,
    ruvia::db_column<"schema_version", std::int32_t>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"manifest_digest", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"manifest_envelope", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text, .nullable_ = true}>,
    ruvia::db_column<"target_node_count", std::int64_t>,
    ruvia::db_column<"created_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"activated_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_node_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"registration_status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"agent_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 32}>,
    ruvia::db_column<"node_secret_hash", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"node_secret_envelope", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text, .nullable_ = true}>,
    ruvia::db_column<"desired_release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .nullable_ = true}>,
    ruvia::db_column<"updated_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_target_entity = ruvia::db_entity<"sys_node_release_target",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"node_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"failed_phase", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 32}>,
    ruvia::db_column<"error_code", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 64}>,
    ruvia::db_column<"last_error", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 1000}>,
    ruvia::db_column<"retryable", bool, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"applied_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"updated_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>>;

using dispatch_mapping_entity = ruvia::db_entity<"sys_cluster_release_object",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"release_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"position", std::int32_t, ruvia::db_column_options{.primary_key_ = true}>,
    ruvia::db_column<"object_digest", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .length_ = 64}>>;

using dispatch_object_entity = ruvia::db_entity<"sys_delivery_object",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"digest_sha256", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::char_value, .primary_key_ = true, .length_ = 64}>,
    ruvia::db_column<"kind", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"payload_envelope", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::text}>,
    ruvia::db_column<"created_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz}>>;

using dispatch_website_entity = ruvia::db_entity<"sys_website",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"revision", std::int64_t>, ruvia::db_column<"sort", std::int64_t>,
    ruvia::db_column<"status", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"config", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_certificate_entity = ruvia::db_entity<"sys_certificate",
    ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"subject_alt_names", std::pmr::vector<std::pmr::string>>,
    ruvia::db_column<"material", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"issued_revision", std::int64_t>,
    ruvia::db_column<"expires_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"deleted_at", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;

using dispatch_binding_entity = ruvia::db_entity<"sys_website_certificate_binding",
    ruvia::db_column<"tenant_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"website_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"certificate_id", std::pmr::string, ruvia::db_column_options{.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"position", std::int32_t>>;

using allocated_cluster = ruvia::db_projection<ruvia::db_column<"generation", std::int64_t>,
    ruvia::db_column<"access_domain", std::pmr::string>, ruvia::db_column<"enabled", bool>>;
using release_identity = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>>;
using target_node_identity = ruvia::db_projection<ruvia::db_column<"node_id", std::pmr::string>>;
using consumer_cluster_identity = ruvia::db_projection<ruvia::db_column<"cluster_id", std::pmr::string>>;
using locked_release = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"schema_version", std::int64_t, ruvia::db_column_options{.nullable_ = true}>>;
using stored_object_payload = ruvia::db_projection<ruvia::db_column<"kind", std::pmr::string>,
    ruvia::db_column<"payload_envelope", std::pmr::string>>;
using published_website = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>,
    ruvia::db_column<"revision", std::int64_t>, ruvia::db_column<"enabled", bool>, ruvia::db_column<"config", std::pmr::string>>;
using published_certificate = ruvia::db_projection<ruvia::db_column<"website_id", std::pmr::string>,
    ruvia::db_column<"id", std::pmr::string>,
    ruvia::db_column<"wildcard_domain", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"base_domain", std::pmr::string, ruvia::db_column_options{.nullable_ = true}>,
    ruvia::db_column<"material", std::pmr::string>>;

}  // namespace service::node_dispatch
