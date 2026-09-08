#pragma once

#include <ruvia/web/Model.h>

namespace service::task {

RUVIA_RESPONSE_MODEL(TaskDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_type", resourceType, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("resource_id", resourceId, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(operation, ruvia::String),
                     RUVIA_REQUIRED_FIELD(version, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_REQUIRED_FIELD(error, ruvia::String),
                     RUVIA_REQUIRED_FIELD(failures, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", nextAttemptAt, ruvia::String));

RUVIA_RESPONSE_MODEL(TaskPageDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<TaskDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(active, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(failed, ruvia::Int64));
RUVIA_RESPONSE_MODEL(TaskPageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, TaskPageDto));
RUVIA_RESPONSE_MODEL(TaskResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, TaskDto));

RUVIA_RESPONSE_MODEL(TaskAttemptDto, RUVIA_REQUIRED_FIELD(outcome, ruvia::String),
                     RUVIA_REQUIRED_FIELD(error, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("emitted_at", emittedAt, ruvia::String));
RUVIA_RESPONSE_MODEL(TaskHistoryDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<TaskAttemptDto>),
                     RUVIA_REQUIRED_FIELD(truncated, ruvia::Bool));
RUVIA_RESPONSE_MODEL(TaskHistoryResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, TaskHistoryDto));

} // namespace service::task
