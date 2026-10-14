#pragma once

#include "ruvia/web/model.h"

namespace service::dns {

RUVIA_MODEL(dns_credential_config_input, RUVIA_OPTIONAL_FIELD(envelope, ruvia::string),
    RUVIA_OPTIONAL_FIELD(hint, ruvia::string));
RUVIA_MODEL(dns_provider_config_input,
    RUVIA_OPTIONAL_FIELD(credential, dns_credential_config_input));
RUVIA_MODEL(dns_credential_config_output, RUVIA_REQUIRED_FIELD(envelope, ruvia::string),
    RUVIA_REQUIRED_FIELD(hint, ruvia::string));
RUVIA_MODEL(dns_provider_config_output,
    RUVIA_REQUIRED_FIELD(credential, dns_credential_config_output));

}  // namespace service::dns
