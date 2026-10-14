#pragma once

#include "ruvia/web/model.h"

namespace service::website_dns {

RUVIA_MODEL(website_domain_runtime_input, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("resolution_status", resolution_status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", last_verified_at, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string));
RUVIA_MODEL(website_runtime_input,
    RUVIA_OPTIONAL_FIELD_NAME("domain_states", domain_states,
        ruvia::array<website_domain_runtime_input>));

RUVIA_MODEL(
    website_domain_runtime_output, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("resolution_status", resolution_status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", last_verified_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(website_runtime_output,
    RUVIA_REQUIRED_FIELD_NAME("domain_states", domain_states,
        ruvia::array<website_domain_runtime_output>));

}  // namespace service::website_dns
