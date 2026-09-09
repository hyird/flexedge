#pragma once

#include <ruvia/web/Model.h>

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(CertificateConfigInput,
                    RUVIA_OPTIONAL_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));
RUVIA_RESPONSE_MODEL(CertificateConfigOutput,
                     RUVIA_REQUIRED_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));
} // namespace service::certificate_issuance
