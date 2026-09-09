#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>

#include "service/domains/auth/session_credential.h"
#include "service/config/auth.h"

namespace service::auth {

inline constexpr std::string_view kSessionCookieName{"flexedge_session"};

inline ruvia::CookieOptions sessionCookieOptions() {
    const auto config = service::config::authConfiguration(ruvia::app().env());
    return {
        .path = "/api",
        .sameSite = ruvia::CookieSameSite::kStrict,
        .maxAge = config.sessionExpiresIn,
        .httpOnly = ruvia::CookieAttributePolicy::kEmit,
        .secure = config.cookieSecure ? ruvia::CookieAttributePolicy::kEmit
                                     : ruvia::CookieAttributePolicy::kOmit,
    };
}

inline void setSessionCookie(ruvia::Context& c, const SessionCredential& credential) {
    const auto value = encodeSessionCredential(credential);
    c.setCookie(
        {.name = kSessionCookieName, .value = value.view(), .attributes = sessionCookieOptions()});
}

inline void deleteSessionCookie(ruvia::Context& c) {
    c.deleteCookie({.name = kSessionCookieName, .attributes = sessionCookieOptions()});
}

inline std::optional<SessionCredential> readSessionCookie(ruvia::Context& c) {
    const auto cookie = c.req().cookie(kSessionCookieName);
    if (!cookie) {
        return std::nullopt;
    }
    return parseSessionCredential(*cookie);
}

} // namespace service::auth
