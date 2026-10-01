#pragma once

#include "ruvia/web/Model.h"

#include <cstdint>

namespace service::common {

inline constexpr ruvia::FixedString kUuidPattern{
    R"(^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$)"};

RUVIA_MODEL(OperationResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String));

RUVIA_MODEL(HealthData, RUVIA_REQUIRED_FIELD(status, ruvia::String));

RUVIA_MODEL(HealthResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, HealthData));

RUVIA_MODEL(CountData,
                     RUVIA_REQUIRED_FIELD_NAME("created_count", createdCount, ruvia::Int64));

RUVIA_MODEL(CountResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, CountData));

RUVIA_MODEL(ErrorResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String));

} // namespace service::common
