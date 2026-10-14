#pragma once

#include <ruvia/web/controller.h>

#include "service/domains/website/website_types.h"
#include "service/features/website_config/validation.h"

namespace service::website {

class website_config_validator final {
public:
    ruvia::task<void> handle(ruvia::context& c, ruvia::next& next) {
        const auto& body = c.req().validated<website_save_input>();
        ruvia::validator validator({.resource_ = c.pool()});
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
        validator.throw_if_invalid();
        co_await next();
    }
};

}  // namespace service::website
