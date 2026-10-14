#pragma once

#include <ruvia/web/controller.h>
#include <ruvia/web/validation.h>

#include <utility>

#include "service/domains/auth/auth_types.h"

namespace service::auth {

class login_validator final {
public:
    void validate(const login_body& body, ruvia::validator& validator) const {
        const auto& username = body.get<"username">();
        validator.required(username, "username", "用户名不能为空");
        validator.min_length(username, "username", 1, "用户名不能为空");
        validator.max_length(username, "username", 50, "用户名最多50个字符");

        const auto& password = body.get<"password">();
        validator.required(password, "password", "密码不能为空");
        validator.min_length(password, "password", 1, "密码不能为空");
        validator.max_length(password, "password", 1024, "密码最多1024个字符");
    }

    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated<login_body>();
        ruvia::validator validator({.resource_ = context.pool()});
        validate(body, validator);
        validator.throw_if_invalid();
        co_await next();
    }
};

}  // namespace service::auth
