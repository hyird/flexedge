#pragma once

#include <cstdint>

#include "ruvia/web/model.h"

namespace service::common {

RUVIA_MODEL(operation_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string));

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
