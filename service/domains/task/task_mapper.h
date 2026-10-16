#pragma once

#include <ruvia/web/model_json.h>

#include <memory_resource>
#include <string_view>

#include "service/domains/task/task_entity.h"
#include "service/domains/task/task_types.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/sync_runtime/attempt_record.h"

namespace service::task {

inline task_dto to_task(const task_projection& stored, std::pmr::memory_resource* pool) {
    task_dto result({.resource_ = pool});
    result.set<"resource_type">(stored.get<"resource_type">());
    result.set<"resource_id">(stored.get<"resource_id">());
    result.set<"resource_kind">(stored.get<"resource_kind">());
    result.set<"name">(stored.get<"name">());
    result.set<"operation">(stored.get<"operation">());
    result.set<"status">(stored.get<"status">());
    result.set<"warning">(stored.get<"warning">());
    result.set<"error">(stored.get<"error">());
    result.set<"next_attempt_at">(stored.get<"next_attempt_at">());
    result.set<"updated_at">(stored.get<"updated_at">());
    result.set<"last_success_at">(stored.get<"last_success_at">());
    result.set<"attempt_count">(stored.get<"attempt_count">());
    result.set<"failure_count">(stored.get<"failure_count">());
    result.set<"node_offline">(stored.get<"node_offline">());
    return result;
}

inline ruvia::json_object parse_object(std::string_view input, std::string_view fallback,
    std::pmr::memory_resource* pool) {
    auto parsed = ruvia::json_object::parse(input, {.resource_ = pool});
    if (!parsed) {
        parsed = ruvia::json_object::parse(fallback, {.resource_ = pool});
    }
    return std::move(*parsed);
}

inline attempt_dto to_attempt(const attempt_projection& stored, std::pmr::memory_resource* pool) {
    attempt_dto result({.resource_ = pool});
    result.set<"id">(stored.get<"id">());
    result.set<"attempt">(stored.get<"attempt">());
    result.set<"operation">(stored.get<"operation">());
    result.set<"outcome">(stored.get<"outcome">());
    result.set<"error">(stored.get<"error">());
    result.set<"started_at">(stored.get<"started_at">());
    result.set<"emitted_at">(stored.get<"emitted_at">());
    result.set<"duration_ms">(stored.get<"duration_ms">());
    result.set<"next_attempt_at">(stored.get<"next_attempt_at">());
    result.set<"detail">(parse_object(stored.get<"detail">(), service::sync_runtime::detail_none_json, pool));
    result.set<"trace">(parse_object(stored.get<"trace">(), service::attempt_trace::summary_none_json, pool));
    return result;
}

inline history_summary_dto to_history_summary(const history_summary_projection& stored,
    std::pmr::memory_resource* pool) {
    history_summary_dto result({.resource_ = pool});
    result.set<"attempt_count">(stored.get<"attempt_count">());
    result.set<"failure_count">(stored.get<"failure_count">());
    result.set<"first_failure_at">(stored.get<"first_failure_at">());
    result.set<"last_success_at">(stored.get<"last_success_at">());
    return result;
}

inline std::string_view worker_type_for(std::string_view resource_type) noexcept {
    if (resource_type == "dns_zone") {
        return "dns-sync";
    }
    if (resource_type == "certificate") {
        return "certificate";
    }
    if (resource_type == "website") {
        return "website-dispatch";
    }
    if (resource_type == "provider") {
        return "provider-verification";
    }
    return "web";
}

inline attempt_trace_dto to_attempt_trace(const attempt_trace_projection& stored, std::pmr::memory_resource* pool) {
    attempt_trace_dto result({.resource_ = pool});
    result.set<"attempt_id">(stored.get<"attempt_id">());
    result.set<"resource_type">(stored.get<"resource_type">());
    result.set<"resource_id">(stored.get<"resource_id">());
    result.set<"kind">(stored.get<"kind">());
    result.set<"state">(stored.is_null<"steps">() ? "none" : "available");
    result.set<"attempt">(stored.get<"attempt">());
    result.set<"outcome">(stored.get<"outcome">());
    result.set<"error">(stored.get<"error">());
    result.set<"started_at">(stored.get<"started_at">());
    result.set<"emitted_at">(stored.get<"emitted_at">());
    result.set<"duration_ms">(stored.get<"duration_ms">());
    result.set<"worker_type">(stored.is_null<"worker_type">() ? worker_type_for(stored.get<"resource_type">())
                                                              : std::string_view(stored.get<"worker_type">()));
    result.set<"capture">(parse_object(stored.is_null<"capture">() ? std::string_view{}
                                                                   : std::string_view(stored.get<"capture">()),
        service::attempt_trace::capture_none_json, pool));
    auto steps = ruvia::json_value::parse(stored.is_null<"steps">() ? std::string_view{"[]"}
                                                                    : std::string_view(stored.get<"steps">()),
        {.resource_ = pool});
    if (!steps || !steps->is_array()) {
        steps = ruvia::json_value::parse(std::string_view{"[]"}, {.resource_ = pool});
    }
    result.set<"steps">(std::move(*steps));
    return result;
}

}  // namespace service::task
