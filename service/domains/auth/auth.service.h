#pragma once

#include <string>
#include <variant>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/common/types.h"
#include "service/domains/auth/auth.error.h"
#include "service/domains/auth/authenticate.service.h"
#include "service/domains/auth/auth_session.service.h"
#include "service/domains/auth/auth.types.h"
#include "service/domains/auth/auth_user.mapper.h"
#include "service/domains/auth/auth_user.store.h"

namespace service::auth {

class AuthService {
  public:
    static AuthService& instance() {
        static AuthService svc;
        return svc;
    }

    ruvia::Task<AuthSessionDto> login(ruvia::Context& c, const LoginBody& req) {
        const auto& usernameInput = req.get<"username">();
        const auto& passwordInput = req.get<"password">();
        if (!usernameInput || !passwordInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "用户名和密码不能为空", 400);
        }
        const std::string username(usernameInput->view());
        const auto password = passwordInput->view();

        const auto user = co_await authenticateUser(c.db(), username, password);
        if (!user) {
            const auto& failure = user.error();
            if (const auto* locked = std::get_if<LoginLocked>(&failure)) {
                const auto minutes = (locked->remainingSeconds + 59) / 60;
                service::common::throwAppError(
                    AuthError::TOO_MANY_ATTEMPTS.code,
                    "登录失败次数过多，请" + std::to_string(minutes) + "分钟后再试", 429);
            }
            if (const auto* rejected = std::get_if<LoginRejected>(&failure)) {
                if (rejected->remainingAttempts > 0) {
                    service::common::throwAppError(
                        AuthError::PASSWORD_INCORRECT.code,
                        "用户名或密码错误，还剩" + std::to_string(rejected->remainingAttempts) + "次尝试机会", 401);
                }
                service::common::throwAppError(AuthError::TOO_MANY_ATTEMPTS);
            }
            service::common::throwAppError(AuthError::USER_DISABLED);
        }

        co_await authSessionService().start(c, user->id);
        AuthSessionDto result(c);
        result.set<"user">(authUserInfo(c, user->id, user->username,
                                      user->nickname, user->status));
        co_return result;
    }

    ruvia::Task<AuthUserInfoDto> getCurrentUser(ruvia::Context& c, const std::string& adminId) {
        const auto user = co_await findAuthUser(c.db(), adminId);
        if (!user) {
            service::common::throwAppError(AuthError::USER_NOT_FOUND);
        }
        if (user->status == "disabled") {
            service::common::throwAppError(AuthError::USER_DISABLED);
        }
        co_return authUserInfo(c, user->id, user->username, user->nickname, user->status);
    }

  private:
    AuthService() = default;
};

inline AuthService& authService() { return AuthService::instance(); }

} // namespace service::auth
