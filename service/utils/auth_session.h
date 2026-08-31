#pragma once

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>

#include "service/utils/sensitive_string.h"

namespace service::auth {

struct SessionCredential final {
    std::string id;
    service::utils::SensitiveString secret;
};

inline constexpr std::string_view kSessionCookieName{"flexedge_session"};

namespace auth_session_detail {

inline std::chrono::seconds parseDuration(std::string_view value, std::string_view configName) {
    if (value.empty()) {
        throw std::runtime_error(std::string(configName) + " must not be empty");
    }

    std::string_view number = value;
    std::int64_t multiplier = 1;
    const char suffix = value.back();
    if (suffix < '0' || suffix > '9') {
        number.remove_suffix(1);
        switch (suffix) {
        case 's':
            multiplier = 1;
            break;
        case 'm':
            multiplier = 60;
            break;
        case 'h':
            multiplier = 60 * 60;
            break;
        case 'd':
            multiplier = 60 * 60 * 24;
            break;
        default:
            throw std::runtime_error(std::string(configName) + " must use an s, m, h, or d suffix");
        }
    }

    std::int64_t count = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), count);
    if (error != std::errc{} || end != number.data() + number.size() || count <= 0 ||
        count > std::numeric_limits<std::int64_t>::max() / multiplier) {
        throw std::runtime_error(std::string(configName) + " must be a positive duration");
    }
    return std::chrono::seconds(count * multiplier);
}

} // namespace auth_session_detail

inline std::chrono::seconds authSessionExpiresIn() {
    return auth_session_detail::parseDuration(
        ruvia::app().env().get("AUTH_SESSION_EXPIRES_IN").value_or("7d"),
        "AUTH_SESSION_EXPIRES_IN");
}

inline bool authCookieSecure() {
    const auto value = ruvia::app().env().get("AUTH_COOKIE_SECURE").value_or("true");
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    throw std::runtime_error("AUTH_COOKIE_SECURE must be true or false");
}

inline void validateAuthSessionConfiguration() {
    (void)authSessionExpiresIn();
    (void)authCookieSecure();
}

inline ruvia::CookieOptions sessionCookieOptions() {
    return {
        .path = "/api",
        .sameSite = ruvia::CookieSameSite::kStrict,
        .maxAge = authSessionExpiresIn(),
        .httpOnly = ruvia::CookieAttributePolicy::kEmit,
        .secure = authCookieSecure() ? ruvia::CookieAttributePolicy::kEmit
                                     : ruvia::CookieAttributePolicy::kOmit,
    };
}

inline void setSessionCookie(ruvia::Context& c, const SessionCredential& credential) {
    auto plainValue = credential.id;
    plainValue.push_back('.');
    plainValue.append(credential.secret.view());
    service::utils::SensitiveString value(std::move(plainValue));
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
    const auto separator = cookie->find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= cookie->size()) {
        return std::nullopt;
    }
    return SessionCredential{
        std::string(cookie->substr(0, separator)),
        service::utils::SensitiveString(std::string(cookie->substr(separator + 1)))};
}

} // namespace service::auth
