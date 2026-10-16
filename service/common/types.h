#pragma once

#include <cstdint>

#include "ruvia/web/model.h"

namespace service::common {

// The task (operation) a resource write started, including writes that need
// no background subtasks (docs/task-center.md).
RUVIA_MODEL(operation_reference_dto, RUVIA_REQUIRED_FIELD_NAME("operation_id", operation_id, ruvia::string));

RUVIA_MODEL(operation_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_OPTIONAL_FIELD(data, operation_reference_dto));

RUVIA_MODEL(health_data, RUVIA_REQUIRED_FIELD(status, ruvia::string));

RUVIA_MODEL(health_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, health_data));

RUVIA_MODEL(count_data,
    RUVIA_REQUIRED_FIELD_NAME("created_count", created_count, ruvia::int64));

RUVIA_MODEL(count_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, count_data));

RUVIA_MODEL(error_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string));

}  // namespace service::common
