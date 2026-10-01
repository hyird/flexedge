#pragma once

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

namespace service::certificate_issuance {

RUVIA_MODEL(CertificateConfigInput,
                    RUVIA_OPTIONAL_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));

inline void validateCertificateConfig(const CertificateConfigInput& config,
                                      ruvia::Validator& validator) {
    validator.required(config.get<"autoRenew">(), "auto_renew", "自动续签设置不能为空");
}
RUVIA_MODEL(CertificateConfigOutput,
                     RUVIA_REQUIRED_FIELD_NAME("auto_renew", autoRenew, ruvia::Bool));
} // namespace service::certificate_issuance
