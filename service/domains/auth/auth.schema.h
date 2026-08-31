#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/types.h"
#include "service/domains/auth/auth.types.h"

namespace service::auth {

class LoginValidator final : public ruvia::Middleware<LoginValidator> {
    RUVIA_VALIDATE_JSON(LoginBody,
                        RUVIA_RULE(username, RUVIA_REQUIRED("用户名不能为空"),
                                   RUVIA_MIN(1, "用户名不能为空"),
                                   RUVIA_MAX(50, "用户名最多50个字符")),
                        RUVIA_RULE(password, RUVIA_REQUIRED("密码不能为空"),
                                   RUVIA_MIN(1, "密码不能为空"),
                                   RUVIA_MAX(1024, "密码最多1024个字符")))
};

} // namespace service::auth
