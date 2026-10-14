#pragma once

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

namespace service::certificate_issuance {

RUVIA_MODEL(certificate_config_input,
    RUVIA_OPTIONAL_FIELD_NAME("auto_renew", auto_renew, ruvia::bool_value));

inline void validate_certificate_config(const certificate_config_input& config,
    ruvia::validator& validator) {
    validator.required(config.get<"auto_renew">(), "auto_renew", "自动续签设置不能为空");
}
RUVIA_MODEL(certificate_config_output,
    RUVIA_REQUIRED_FIELD_NAME("auto_renew", auto_renew, ruvia::bool_value));
}  // namespace service::certificate_issuance
