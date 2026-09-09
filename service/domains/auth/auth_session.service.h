#pragma once

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>

#include "service/common/http.h"
#include "service/config/auth.h"
#include <ruvia/web/App.h>
#include "service/domains/auth/auth.error.h"
#include "service/domains/auth/auth.types.h"
#include "service/domains/auth/auth_session.store.h"
#include "service/domains/auth/auth_user.mapper.h"
#include "service/domains/auth/session_cookie.h"

namespace service::auth {

class AuthSessionService final {
  public:
    ruvia::Task<void> start(ruvia::Context& c, const std::string& adminId) const {
        const auto configuration = service::config::authConfiguration(ruvia::app().env());
        const auto session = co_await createSessionCredential(c.db(), adminId, configuration.sessionExpiresIn);
        setSessionCookie(c, session);
    }

    ruvia::Task<AuthSessionDto> refresh(ruvia::Context& c) const {
        const auto presented = readSessionCookie(c);
        if (!presented) {
            service::common::throwAppError(AuthError::UNAUTHORIZED);
        }
        const auto configuration = service::config::authConfiguration(ruvia::app().env());
        auto rotated = co_await rotateSession(c.db(), *presented, configuration.sessionExpiresIn);
        if (!rotated) {
            if (rotated.error() != SessionRotationError::UnknownSession) deleteSessionCookie(c);
            service::common::throwAppError(rotated.error() == SessionRotationError::UserDisabled
                ? AuthError::USER_DISABLED : AuthError::SESSION_INVALID);
        }
        setSessionCookie(c, rotated->credential);
        AuthSessionDto result(c);
        result.set<"user">(authUserInfo(c, rotated->user.id, rotated->user.username,
                                      rotated->user.nickname, rotated->user.status));
        co_return result;
    }

    ruvia::Task<void> logout(ruvia::Context& c) const {
        const auto presented = readSessionCookie(c);
        if (presented) co_await revokeSessionFamily(c.db(), *presented);
        deleteSessionCookie(c);
    }
};

inline const AuthSessionService& authSessionService() {
    static const AuthSessionService service;
    return service;
}

} // namespace service::auth
