#pragma once

#include "ruvia/web/model.h"

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

RUVIA_MODEL(task_attempt_dto, RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("emitted_at", emitted_at, ruvia::string));
RUVIA_MODEL(task_history_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<task_attempt_dto>),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value));
RUVIA_MODEL(task_history_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, task_history_dto));

}  // namespace service::task
