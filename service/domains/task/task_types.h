#pragma once

#include "ruvia/web/model.h"
#include "ruvia/web/model_object.h"

namespace service::task {

RUVIA_MODEL(task_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_type", resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_id", resource_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(operation, ruvia::string),
    RUVIA_REQUIRED_FIELD(version, ruvia::int64),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(failures, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", next_attempt_at, ruvia::string));

RUVIA_MODEL(task_page_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<task_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64),
    RUVIA_REQUIRED_FIELD(active, ruvia::int64),
    RUVIA_REQUIRED_FIELD(failed, ruvia::int64));
RUVIA_MODEL(task_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_page_dto));
RUVIA_MODEL(task_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_dto));

// `detail` is the stored attempt detail document; its `kind` selects the
// shape documented in docs/task-center.md. `trace` is the step summary of the
// attempt trace plus its `attempt_id` ("state": "none" without a trace).
RUVIA_MODEL(task_attempt_dto, RUVIA_REQUIRED_FIELD(attempt, ruvia::int64),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("emitted_at", emitted_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(detail, ruvia::json_object),
    RUVIA_REQUIRED_FIELD(trace, ruvia::json_object));
// Covers every retained attempt of the task version, not only the listed 100.
RUVIA_MODEL(task_history_summary_dto, RUVIA_REQUIRED_FIELD(attempt_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(failure_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(first_failure_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(last_success_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(next_attempt_at, ruvia::string));
RUVIA_MODEL(task_history_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<task_attempt_dto>),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(summary, task_history_summary_dto));
RUVIA_MODEL(task_history_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_history_dto));

// Full trace of one attempt: the attempt fields from its history row plus
// the stored capture metadata and steps (empty when `state` is "none").
RUVIA_MODEL(task_attempt_trace_dto, RUVIA_REQUIRED_FIELD(attempt_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(task_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(resource_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(state, ruvia::string),
    RUVIA_REQUIRED_FIELD(attempt, ruvia::int64),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(emitted_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(worker_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(capture, ruvia::json_object),
    RUVIA_REQUIRED_FIELD(steps, ruvia::json_value));
RUVIA_MODEL(task_attempt_trace_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_attempt_trace_dto));

}  // namespace service::task
