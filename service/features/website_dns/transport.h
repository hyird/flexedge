#pragma once

#include "ruvia/web/Model.h"


namespace service::website_dns {

RUVIA_MODEL(WebsiteDomainRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("resolution_status", resolutionStatus, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));
RUVIA_MODEL(WebsiteRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("domain_states", domainStates,
                                              ruvia::Array<WebsiteDomainRuntimeInput>));

RUVIA_MODEL(
    WebsiteDomainRuntimeOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resolution_status", resolutionStatus, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(WebsiteRuntimeOutput,
                     RUVIA_REQUIRED_FIELD_NAME("domain_states", domainStates,
                                               ruvia::Array<WebsiteDomainRuntimeOutput>));

} // namespace service::website_dns
