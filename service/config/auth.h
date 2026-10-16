#pragma once

#include <ruvia/web/dotenv.h>

#include <chrono>
#include <optional>
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

inline std::optional<auth_configuration>& auth_storage() {
    static std::optional<auth_configuration> config;
    return config;
}

// Set once before starting workers; every subsequent access is read-only.
inline void configure_auth(auth_configuration config) {
    auto& storage = auth_storage();
    if (storage) {
        throw std::logic_error("auth already configured");
    }
    storage.emplace(config);
}

inline const auth_configuration& auth_config() {
    const auto& storage = auth_storage();
    if (!storage) {
        throw std::logic_error("auth is not configured");
    }
    return *storage;
}

}  // namespace service::config
