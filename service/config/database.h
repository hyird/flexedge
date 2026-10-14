#pragma once

#include <ruvia/web/db/db_types.h>
#include <ruvia/web/dotenv.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::config {

namespace database_detail {

inline std::string require_value(const ruvia::env& env, std::string_view name) {
    const auto value = env.get(name);
    if (!value || value->empty()) {
        throw std::runtime_error(std::string(name) + " environment variable is required");
    }
    return std::string(*value);
}

}  // namespace database_detail

inline ruvia::db_config database_config(const ruvia::env& env) {
    const auto port = env.get<std::uint16_t>("DB_PORT");
    if (!port || *port == 0) {
        throw std::runtime_error("DB_PORT must be a positive TCP port");
    }

    return {
        .driver_ = ruvia::db_driver::postgresql,
        .host_ = database_detail::require_value(env, "DB_HOST"),
        .port_ = port.value(),
        .username_ = database_detail::require_value(env, "DB_USERNAME"),
        .password_ = database_detail::require_value(env, "DB_PASSWORD"),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .database_ = database_detail::require_value(env, "DB_DATABASE"),
    };
}

}  // namespace service::config
