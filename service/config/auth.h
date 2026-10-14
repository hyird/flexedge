#pragma once

#include <ruvia/web/dotenv.h>

#include <chrono>
#include <stdexcept>

#include "service/config/duration.h"

namespace service::config {

struct auth_configuration final {
    std::chrono::seconds session_expires_in_;
    bool cookie_secure_;
};

inline auth_configuration get_auth_configuration(const ruvia::env& env) {
    const auto expires_in = parse_positive_duration(
        env.get("AUTH_SESSION_EXPIRES_IN").value_or("7d"), "AUTH_SESSION_EXPIRES_IN");
    const auto secure = env.get("AUTH_COOKIE_SECURE").value_or("true");
    if (secure != "true" && secure != "false") {
        throw std::runtime_error("AUTH_COOKIE_SECURE must be true or false");
    }
    return {expires_in, secure == "true"};
}

}  // namespace service::config
