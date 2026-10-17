#pragma once

#include <ruvia/web/controller.h>

#include "service/common/http.h"
#include "service/domains/website/website_types.h"
#include "service/features/website_config/validation.h"

namespace service::website {

inline void validate_website_save_input(const website_save_input& body, ruvia::validator& validator) {
    const auto& status = body.get<"status">();
    validator.required(status, "status", "网站状态不能为空");
    if (status && status->view() != "enabled" && status->view() != "disabled") {
        validator.add("status", "regex", "网站状态不正确");
    }
    const auto& config = body.get<"config">();
    validator.required(config, "config", "网站配置不能为空");
    if (config) {
        service::website_config::website_config_rules{}.validate_nested(*config, "config",
            validator);
    }
}

using website_config_validator =
    service::common::json_validator_middleware<website_save_input, validate_website_save_input>;

}  // namespace service::website
