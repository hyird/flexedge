#pragma once

#include "ruvia/web/model.h"
#include "ruvia/web/model_object.h"

// Task center contract: one task per (resource_type, resource_id), the
// resource id being the task id; re-initiating the work resets the same task.
// Times are `YYYY-MM-DDTHH:MM:SS.ffffff+00` or "" when unknown.
namespace service::task {

// `resource_kind` is the provider kind ("dns" | "certificate") of provider
// tasks, "" otherwise. `attempt_count` covers every retained attempt;
// `failure_count` is current retry state and clears on success or re-initiation.
// Cumulative failures belong to the execution-history summary.
RUVIA_MODEL(task_dto, RUVIA_REQUIRED_FIELD_NAME("resource_type", resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_id", resource_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resource_kind", resource_kind, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(operation, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", next_attempt_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("last_success_at", last_success_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("attempt_count", attempt_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("failure_count", failure_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("node_offline", node_offline, ruvia::bool_value));
RUVIA_MODEL(task_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64), RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_dto));

// `active` and `failed` count every task of the tenant, not only the filtered ones.
RUVIA_MODEL(task_page_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<task_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64), RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64), RUVIA_REQUIRED_FIELD(active, ruvia::int64),
    RUVIA_REQUIRED_FIELD(failed, ruvia::int64));
RUVIA_MODEL(task_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, task_page_dto));

// `id` is the attempt id the trace endpoint takes; `attempt` numbers the
// attempts of one piece of work from 1. `detail` is the stored attempt detail
// document (`kind` selects its shape); `trace` is the step summary with
// `attempt_id`.
RUVIA_MODEL(attempt_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string), RUVIA_REQUIRED_FIELD(attempt, ruvia::int64),
    RUVIA_REQUIRED_FIELD(operation, ruvia::string), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("emitted_at", emitted_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(detail, ruvia::json_object), RUVIA_REQUIRED_FIELD(trace, ruvia::json_object));
// Covers every retained attempt, not live task scheduling.
RUVIA_MODEL(history_summary_dto, RUVIA_REQUIRED_FIELD(attempt_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(failure_count, ruvia::int64), RUVIA_REQUIRED_FIELD(first_failure_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(last_success_at, ruvia::string));
RUVIA_MODEL(task_history_dto, RUVIA_REQUIRED_FIELD(summary, history_summary_dto),
    RUVIA_REQUIRED_FIELD(list, ruvia::array<attempt_dto>));
RUVIA_MODEL(task_history_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, task_history_dto));
// The `attempt` event of the detail stream: one attempt recorded after the
// history snapshot, with the task's new summary.
RUVIA_MODEL(task_attempt_event_dto, RUVIA_REQUIRED_FIELD(item, attempt_dto),
    RUVIA_REQUIRED_FIELD(summary, history_summary_dto));

// Full trace of one attempt: the attempt fields plus the stored capture
// metadata and steps (empty when `state` is "none").
RUVIA_MODEL(attempt_trace_dto, RUVIA_REQUIRED_FIELD(attempt_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(resource_type, ruvia::string), RUVIA_REQUIRED_FIELD(resource_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(state, ruvia::string),
    RUVIA_REQUIRED_FIELD(attempt, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(emitted_at, ruvia::string), RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(worker_type, ruvia::string), RUVIA_REQUIRED_FIELD(capture, ruvia::json_object),
    RUVIA_REQUIRED_FIELD(steps, ruvia::json_value));
RUVIA_MODEL(attempt_trace_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(data, attempt_trace_dto));

}  // namespace service::task
