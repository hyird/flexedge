#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/website/website.types.h"
#include "service/features/website_config/validation.h"

namespace service::website {

class WebsiteConfigValidator final : public ruvia::Middleware {
  public:
    ruvia::Task<void> handle(ruvia::Context& c, ruvia::Next& next) {
        const auto& body = c.req().validated<WebsiteSaveInput>();
        ruvia::Validator validator({.resource = c.pool()});
        const auto& status = body.get<"status">();
        validator.required(status, "status", "网站状态不能为空");
        if (status && status->view() != "enabled" && status->view() != "disabled") {
            validator.add("status", "regex", "网站状态不正确");
        }
        const auto& config = body.get<"config">();
        validator.required(config, "config", "网站配置不能为空");
        if (config)
            service::website_config::WebsiteConfigRules{}.validateNested(*config, "config",
                                                                         validator);
        validator.throwIfInvalid();
        co_await next();
    }
};

} // namespace service::website
