#pragma once

#include <ruvia/web/db/db_entity.h>
#include <ruvia/web/db/db_projection.h>

#include <cstdint>
#include <string>

namespace service::task {

// Read-only mappings of the columns used by the task read model. These are
// persistence contracts; the HTTP models remain in task_types.h.
using marker_entity = ruvia::db_entity<"sys_sync_task",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"resource_type", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>,
    ruvia::db_column<"resource_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .generated_type_ = ruvia::db_generated_type::stored, .nullable_ = true}>,
    ruvia::db_column<"operation", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>, ruvia::db_column<"version", std::int64_t>,
    ruvia::db_column<"is_done", bool>, ruvia::db_column<"is_ok", bool>,
    ruvia::db_column<"lease_until", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"count_fails", std::int32_t>,
    ruvia::db_column<"error", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 1000}>,
    ruvia::db_column<"next_attempt_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"updated_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>>;

using website_entity = ruvia::db_entity<"sys_website",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"config", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>>;
using zone_entity = ruvia::db_entity<"sys_dns_zone",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"domain", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 253}>,
    ruvia::db_column<"sync_status", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>>;
using certificate_entity = ruvia::db_entity<"sys_certificate",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"domain", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 253}>>;
using provider_entity = ruvia::db_entity<"sys_provider",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"kind", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"name", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 100}>,
    ruvia::db_column<"provider", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>>;
using node_entity = ruvia::db_entity<"sys_node",
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"cluster_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"name", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 100}>,
    ruvia::db_column<"node_spec_revision", std::int64_t>,
    ruvia::db_column<"applied_node_spec_revision", std::int64_t>,
    ruvia::db_column<"registration_status", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"registered_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"last_heartbeat_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"created_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"updated_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"deleted_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>>;
using release_entity = ruvia::db_entity<"sys_cluster_release",
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"cluster_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"generation", std::int64_t>>;
using release_target_entity = ruvia::db_entity<"sys_node_release_target",
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"release_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"node_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid, .primary_key_ = true}>,
    ruvia::db_column<"status", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"retryable", bool, {.nullable_ = true}>,
    ruvia::db_column<"last_error", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .nullable_ = true, .length_ = 1000}>,
    ruvia::db_column<"updated_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>>;
// generated_ marks the identity for this read mapping; the baseline owns its
// GENERATED ALWAYS enforcement. These partial entities do not create tables.
using event_entity = ruvia::db_entity<"sys_sync_event",
    ruvia::db_column<"id", std::int64_t, {.primary_key_ = true, .generated_ = true}>,
    ruvia::db_column<"tenant_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"resource_type", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>,
    ruvia::db_column<"resource_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"task_id", std::pmr::string, {.data_type_ = ruvia::db_data_type::uuid}>,
    ruvia::db_column<"version", std::int64_t>, ruvia::db_column<"attempt", std::int32_t>,
    ruvia::db_column<"operation", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 32}>,
    ruvia::db_column<"outcome", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 16}>,
    ruvia::db_column<"error", std::pmr::string, {.data_type_ = ruvia::db_data_type::varchar, .length_ = 1000}>,
    ruvia::db_column<"started_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz, .nullable_ = true}>,
    ruvia::db_column<"emitted_at", std::pmr::string, {.data_type_ = ruvia::db_data_type::timestamp_tz}>,
    ruvia::db_column<"duration_ms", std::int64_t>,
    ruvia::db_column<"detail", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>>;
using event_trace_entity = ruvia::db_entity<"sys_sync_event_trace",
    ruvia::db_column<"event_id", std::int64_t, {.primary_key_ = true}>,
    ruvia::db_column<"summary", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>,
    ruvia::db_column<"trace", std::pmr::string, {.data_type_ = ruvia::db_data_type::jsonb}>>;

using task_projection = ruvia::db_projection<
    ruvia::db_column<"resource_type", std::pmr::string>, ruvia::db_column<"resource_id", std::pmr::string>,
    ruvia::db_column<"resource_kind", std::pmr::string>, ruvia::db_column<"name", std::pmr::string>,
    ruvia::db_column<"operation", std::pmr::string>, ruvia::db_column<"status", std::pmr::string>,
    ruvia::db_column<"error", std::pmr::string>,
    ruvia::db_column<"next_attempt_at", std::pmr::string>, ruvia::db_column<"updated_at", std::pmr::string>,
    ruvia::db_column<"last_success_at", std::pmr::string>, ruvia::db_column<"attempt_count", std::int64_t>,
    ruvia::db_column<"failure_count", std::int64_t>, ruvia::db_column<"node_offline", bool>>;
using task_counts_projection = ruvia::db_projection<ruvia::db_column<"total", std::int64_t>,
    ruvia::db_column<"active", std::int64_t>, ruvia::db_column<"failed", std::int64_t>>;
using attempt_projection = ruvia::db_projection<ruvia::db_column<"id", std::pmr::string>,
    ruvia::db_column<"attempt", std::int64_t>, ruvia::db_column<"operation", std::pmr::string>,
    ruvia::db_column<"outcome", std::pmr::string>, ruvia::db_column<"error", std::pmr::string>,
    ruvia::db_column<"started_at", std::pmr::string>, ruvia::db_column<"emitted_at", std::pmr::string>,
    ruvia::db_column<"duration_ms", std::int64_t>,
    ruvia::db_column<"detail", std::pmr::string>, ruvia::db_column<"trace", std::pmr::string>>;
using history_summary_projection = ruvia::db_projection<ruvia::db_column<"attempt_count", std::int64_t>,
    ruvia::db_column<"failure_count", std::int64_t>, ruvia::db_column<"first_failure_at", std::pmr::string>,
    ruvia::db_column<"last_success_at", std::pmr::string>>;
using attempt_trace_projection = ruvia::db_projection<ruvia::db_column<"attempt_id", std::pmr::string>,
    ruvia::db_column<"resource_type", std::pmr::string>, ruvia::db_column<"resource_id", std::pmr::string>,
    ruvia::db_column<"kind", std::pmr::string>, ruvia::db_column<"attempt", std::int64_t>,
    ruvia::db_column<"outcome", std::pmr::string>, ruvia::db_column<"error", std::pmr::string>,
    ruvia::db_column<"started_at", std::pmr::string>, ruvia::db_column<"emitted_at", std::pmr::string>,
    ruvia::db_column<"duration_ms", std::int64_t>,
    ruvia::db_column<"worker_type", std::pmr::string, {.nullable_ = true}>,
    ruvia::db_column<"capture", std::pmr::string, {.nullable_ = true}>,
    ruvia::db_column<"steps", std::pmr::string, {.nullable_ = true}>>;

}  // namespace service::task
