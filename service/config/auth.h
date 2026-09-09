#pragma once

#include <chrono>
#include <stdexcept>
#include <ruvia/web/Dotenv.h>
#include "service/config/duration.h"

namespace service::config {

struct AuthConfiguration final {
    std::chrono::seconds sessionExpiresIn;
    bool cookieSecure;
};

inline AuthConfiguration authConfiguration(const ruvia::Env& env) {
    const auto expiresIn = parsePositiveDuration(
        env.get("AUTH_SESSION_EXPIRES_IN").value_or("7d"), "AUTH_SESSION_EXPIRES_IN");
    const auto secure = env.get("AUTH_COOKIE_SECURE").value_or("true");
    if (secure != "true" && secure != "false") {
        throw std::runtime_error("AUTH_COOKIE_SECURE must be true or false");
    }
    return {expiresIn, secure == "true"};
}

} // namespace service::config
