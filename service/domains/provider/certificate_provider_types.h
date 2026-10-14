#pragma once

#include "ruvia/web/model.h"

namespace service::provider {

RUVIA_MODEL(create_certificate_provider_body,
    RUVIA_OPTIONAL_FIELD(provider, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credential_mode, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", account_email, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("access_key", access_key, ruvia::string));
RUVIA_MODEL(update_certificate_provider_body,
    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credential_mode, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", account_email, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("access_key", access_key, ruvia::string));
RUVIA_MODEL(
    certificate_provider_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64), RUVIA_REQUIRED_FIELD(provider, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("credential_mode", credential_mode, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", account_email, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("access_key_hint", access_key_hint, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", last_verified_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));
RUVIA_MODEL(certificate_provider_list_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, ruvia::array<certificate_provider_dto>));

}  // namespace service::provider
