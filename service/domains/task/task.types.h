#pragma once

#include <ruvia/web/Model.h>

namespace service::task {

RUVIA_RESPONSE_MODEL(
    TaskDto, RUVIA_REQUIRED_FIELD(id, ruvia::String), RUVIA_REQUIRED_FIELD(sequence, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(kind, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resource_type", resourceType, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resource_id", resourceId, ruvia::String),
    RUVIA_REQUIRED_FIELD(operation, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resource_name", resourceName, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String), RUVIA_REQUIRED_FIELD(version, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("processed_version", processedVersion, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("count_fails", countFails, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("next_attempt_at", nextAttemptAt, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("lease_until", leaseUntil, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(error, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("completed_at", completedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(TaskSummaryDto, RUVIA_REQUIRED_FIELD(pending, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(running, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(retry, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(completed, ruvia::Int64));

RUVIA_RESPONSE_MODEL(TaskPageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<TaskDto>),
                     RUVIA_REQUIRED_FIELD(summary, TaskSummaryDto),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));

RUVIA_RESPONSE_MODEL(TaskPageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, TaskPageDataDto));

} // namespace service::task
