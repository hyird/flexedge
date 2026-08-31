#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/Dotenv.h>
#include <ruvia/web/db/DbTypes.h>

namespace service::config {

namespace database_detail {

inline std::string requireValue(const ruvia::Env& env, std::string_view name) {
    const auto value = env.get(name);
    if (!value || value->empty()) {
        throw std::runtime_error(std::string(name) + " environment variable is required");
    }
    return std::string(*value);
}

} // namespace database_detail

inline ruvia::DbConfig databaseConfig(const ruvia::Env& env) {
    const auto port = env.get<std::uint16_t>("DB_PORT");
    if (!port || *port == 0) {
        throw std::runtime_error("DB_PORT must be a positive TCP port");
    }

    return {
        .driver = ruvia::DbDriver::kPostgreSql,
        .host = database_detail::requireValue(env, "DB_HOST"),
        .port = port.value(),
        .username = database_detail::requireValue(env, "DB_USERNAME"),
        .password = database_detail::requireValue(env, "DB_PASSWORD"),
        .database = database_detail::requireValue(env, "DB_DATABASE"),
    };
}

} // namespace service::config
