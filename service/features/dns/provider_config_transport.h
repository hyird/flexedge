#pragma once

#include <ruvia/web/Model.h>

namespace service::dns {

RUVIA_REQUEST_MODEL(DnsCredentialConfigInput, RUVIA_OPTIONAL_FIELD(envelope, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hint, ruvia::String));
RUVIA_REQUEST_MODEL(DnsProviderConfigInput,
                    RUVIA_OPTIONAL_FIELD(credential, DnsCredentialConfigInput));
RUVIA_RESPONSE_MODEL(DnsCredentialConfigOutput, RUVIA_REQUIRED_FIELD(envelope, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hint, ruvia::String));
RUVIA_RESPONSE_MODEL(DnsProviderConfigOutput,
                     RUVIA_REQUIRED_FIELD(credential, DnsCredentialConfigOutput));


} // namespace service::dns
