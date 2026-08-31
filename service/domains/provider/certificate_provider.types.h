#pragma once

#include <ruvia/web/Model.h>

namespace service::provider {

RUVIA_REQUEST_MODEL(CreateCertificateProviderBody, RUVIA_OPTIONAL_FIELD(provider, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey, ruvia::String));
RUVIA_REQUEST_MODEL(UpdateCertificateProviderBody,
                    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("access_key", accessKey, ruvia::String));
RUVIA_RESPONSE_MODEL(
    CertificateProviderDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD(revision, ruvia::Int64), RUVIA_REQUIRED_FIELD(provider, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("credential_mode", credentialMode, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", accountEmail, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("access_key_hint", accessKeyHint, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));
RUVIA_RESPONSE_MODEL(CertificateProviderListResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, ruvia::Array<CertificateProviderDto>));

} // namespace service::provider
