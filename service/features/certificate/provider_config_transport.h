#pragma once

#include "ruvia/web/model.h"

namespace service::certificate_issuance {

RUVIA_MODEL(certificate_provider_access_key_input,
    RUVIA_OPTIONAL_FIELD(envelope, ruvia::string),
    RUVIA_OPTIONAL_FIELD(hint, ruvia::string));
RUVIA_MODEL(certificate_provider_config_input,
    RUVIA_OPTIONAL_FIELD_NAME("credential_mode", credential_mode, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", account_email, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("access_key", access_key,
        certificate_provider_access_key_input));
RUVIA_MODEL(certificate_provider_access_key_output,
    RUVIA_REQUIRED_FIELD(envelope, ruvia::string),
    RUVIA_REQUIRED_FIELD(hint, ruvia::string));
RUVIA_MODEL(certificate_provider_config_output,
    RUVIA_REQUIRED_FIELD_NAME("credential_mode", credential_mode, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_email", account_email, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("access_key", access_key,
        certificate_provider_access_key_output,
        RUVIA_OMIT_EMPTY));
RUVIA_MODEL(certificate_provider_eab_runtime_input, RUVIA_OPTIONAL_FIELD(kid, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("hmac_key_envelope", hmac_key_envelope, ruvia::string));
RUVIA_MODEL(certificate_provider_account_runtime_input,
    RUVIA_OPTIONAL_FIELD_NAME("private_key_envelope", private_key_envelope,
        ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("account_url", account_url, ruvia::string));
RUVIA_MODEL(certificate_provider_runtime_input,
    RUVIA_OPTIONAL_FIELD(eab, certificate_provider_eab_runtime_input),
    RUVIA_OPTIONAL_FIELD_NAME("acme_account", acme_account,
        certificate_provider_account_runtime_input));
RUVIA_MODEL(certificate_provider_eab_runtime_output, RUVIA_REQUIRED_FIELD(kid, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("hmac_key_envelope", hmac_key_envelope,
        ruvia::string));
RUVIA_MODEL(certificate_provider_account_runtime_output,
    RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", private_key_envelope,
        ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("account_url", account_url, ruvia::string));
RUVIA_MODEL(certificate_provider_runtime_output,
    RUVIA_OPTIONAL_FIELD(eab, certificate_provider_eab_runtime_output,
        RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("acme_account", acme_account,
        certificate_provider_account_runtime_output,
        RUVIA_OMIT_EMPTY));

}  // namespace service::certificate_issuance
