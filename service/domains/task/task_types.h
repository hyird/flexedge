#pragma once

#include "ruvia/web/model.h"
#include "ruvia/web/model_object.h"

// Task center contract (build/operation-tasks/contract.md, docs/task-center.md):
// a task is one user operation or system action, its subtasks the background
// work it caused. Times are `YYYY-MM-DDTHH:MM:SS.ffffff+00` or "" when unknown.
namespace service::task {

RUVIA_MODEL(operation_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_type", target_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_id", target_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_name", target_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_deleted", target_deleted, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("target_kind", target_kind, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("actor_type", actor_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("actor_name", actor_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("finished_at", finished_at, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("subtask_count", subtask_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("settled_count", settled_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("failed_count", failed_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(summary, ruvia::json_object));

RUVIA_MODEL(operation_page_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<operation_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64), RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64), RUVIA_REQUIRED_FIELD(active, ruvia::int64),
    RUVIA_REQUIRED_FIELD(attention, ruvia::int64));
RUVIA_MODEL(operation_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, operation_page_dto));

// The subtask that took over a merged subtask's work, with its current status.
RUVIA_MODEL(subtask_merged_into_dto, RUVIA_REQUIRED_FIELD_NAME("operation_id", operation_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("subtask_id", subtask_id, ruvia::string), RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_name", target_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));
// The task that cancelled (or merged without taking over) a subtask.
RUVIA_MODEL(subtask_closed_by_dto, RUVIA_REQUIRED_FIELD_NAME("operation_id", operation_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(action, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_name", target_name, ruvia::string));

RUVIA_MODEL(subtask_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("parent_id", parent_id, ruvia::string), RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_type", resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_id", resource_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_name", resource_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_deleted", resource_deleted, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(status, ruvia::string), RUVIA_REQUIRED_FIELD(warning, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_offline", node_offline, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("running_since", running_since, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("finished_at", finished_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("duration_ms", duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("attempt_count", attempt_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("failure_count", failure_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(recovered, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("merged_into", merged_into, subtask_merged_into_dto),
    RUVIA_OPTIONAL_FIELD_NAME("closed_by", closed_by, subtask_closed_by_dto));

// operation_dto plus its subtasks (parents before children, by creation).
RUVIA_MODEL(operation_detail_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(action, ruvia::string), RUVIA_REQUIRED_FIELD_NAME("target_type", target_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_id", target_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_name", target_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("target_deleted", target_deleted, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("target_kind", target_kind, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("actor_type", actor_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("actor_name", actor_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("finished_at", finished_at, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("subtask_count", subtask_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("settled_count", settled_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("failed_count", failed_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(summary, ruvia::json_object),
    RUVIA_REQUIRED_FIELD(subtasks, ruvia::array<subtask_dto>));
RUVIA_MODEL(operation_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, operation_detail_dto));

// `attempt` numbers the attempts of one subtask from 1. `detail` is the
// stored attempt detail document (its `kind` selects the shape in
// docs/task-center.md); `trace` the step summary with `attempt_id`.
RUVIA_MODEL(attempt_dto, RUVIA_REQUIRED_FIELD(attempt, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("emitted_at", emitted_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(detail, ruvia::json_object), RUVIA_REQUIRED_FIELD(trace, ruvia::json_object));
// Covers every retained attempt of the subtask, not only the listed 100.
RUVIA_MODEL(history_summary_dto, RUVIA_REQUIRED_FIELD(attempt_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(failure_count, ruvia::int64), RUVIA_REQUIRED_FIELD(first_failure_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(last_success_at, ruvia::string), RUVIA_REQUIRED_FIELD(next_attempt_at, ruvia::string));
RUVIA_MODEL(subtask_history_dto, RUVIA_REQUIRED_FIELD_NAME("subtask_id", subtask_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(list, ruvia::array<attempt_dto>), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(summary, history_summary_dto));
RUVIA_MODEL(subtask_history_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, subtask_history_dto));
// The `attempt` event of the detail stream: one attempt recorded after the
// history snapshot, with the subtask's new summary.
RUVIA_MODEL(subtask_attempt_event_dto, RUVIA_REQUIRED_FIELD_NAME("subtask_id", subtask_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(item, attempt_dto), RUVIA_REQUIRED_FIELD(summary, history_summary_dto));

// Full trace of one attempt: the attempt fields plus the stored capture
// metadata and steps (empty when `state` is "none").
RUVIA_MODEL(attempt_trace_dto, RUVIA_REQUIRED_FIELD(attempt_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(subtask_id, ruvia::string), RUVIA_REQUIRED_FIELD(resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(resource_id, ruvia::string), RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(state, ruvia::string), RUVIA_REQUIRED_FIELD(attempt, ruvia::int64),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string), RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(emitted_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(worker_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(capture, ruvia::json_object), RUVIA_REQUIRED_FIELD(steps, ruvia::json_value));
RUVIA_MODEL(attempt_trace_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, attempt_trace_dto));

}  // namespace service::task
