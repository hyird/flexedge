#pragma once

#include "ruvia/web/Model.h"


namespace service::dns {

RUVIA_MODEL(DnsCredentialConfigInput, RUVIA_OPTIONAL_FIELD(envelope, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hint, ruvia::String));
RUVIA_MODEL(DnsProviderConfigInput,
                    RUVIA_OPTIONAL_FIELD(credential, DnsCredentialConfigInput));
RUVIA_MODEL(DnsCredentialConfigOutput, RUVIA_REQUIRED_FIELD(envelope, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hint, ruvia::String));
RUVIA_MODEL(DnsProviderConfigOutput,
                     RUVIA_REQUIRED_FIELD(credential, DnsCredentialConfigOutput));


} // namespace service::dns
