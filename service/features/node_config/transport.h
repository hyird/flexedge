#pragma once

#include <ruvia/web/Model.h>

namespace service::node_config {

RUVIA_REQUEST_MODEL(NodeEndpointInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("ip_address", ipAddress, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_REQUEST_MODEL(NodeConfigInput,
                    RUVIA_OPTIONAL_FIELD(endpoints, ruvia::Array<NodeEndpointInput>));
RUVIA_RESPONSE_MODEL(NodeEndpointOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("ip_address", ipAddress, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_RESPONSE_MODEL(NodeConfigOutput,
                     RUVIA_REQUIRED_FIELD(endpoints, ruvia::Array<NodeEndpointOutput>));

} // namespace service::node_config
