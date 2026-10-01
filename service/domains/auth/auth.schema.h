#pragma once

#include <utility>

#include <ruvia/web/Controller.h>
#include <ruvia/web/Validation.h>

#include "service/domains/auth/auth.types.h"

namespace service::auth {

class LoginValidator final : public ruvia::Middleware {
  public:
    void validate(const LoginBody& body, ruvia::Validator& validator) const {
        const auto& username = body.get<"username">();
        validator.required(username, "username", "用户名不能为空");
        validator.minLength(username, "username", 1, "用户名不能为空");
        validator.maxLength(username, "username", 50, "用户名最多50个字符");

        const auto& password = body.get<"password">();
        validator.required(password, "password", "密码不能为空");
        validator.minLength(password, "password", 1, "密码不能为空");
        validator.maxLength(password, "password", 1024, "密码最多1024个字符");
    }

    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validated<LoginBody>();
        ruvia::Validator validator({.resource = context.pool()});
        validate(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

} // namespace service::auth
