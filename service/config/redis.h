#pragma once

#include <ruvia/web/dotenv.h>
#include <ruvia/web/redis/redis_types.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace service::config {

inline ruvia::redis_config redis_config(const ruvia::env& env) {
    const auto port = env.get<std::uint16_t>("REDIS_PORT").value_or(6379);
    if (port == 0) {
        throw std::runtime_error("REDIS_PORT must be a positive TCP port");
    }
    const auto database = env.get<std::uint32_t>("REDIS_DATABASE").value_or(0);
    const auto pool_size = env.get<std::size_t>("REDIS_POOL_SIZE_PER_WORKER").value_or(2);
    if (pool_size == 0 || pool_size > 256) {
        throw std::runtime_error("REDIS_POOL_SIZE_PER_WORKER is outside the supported range");
    }
    const auto blocking_pool_size =
        env.get<std::size_t>("REDIS_BLOCKING_POOL_SIZE_PER_WORKER").value_or(1);
    if (blocking_pool_size == 0 || blocking_pool_size > 256) {
        throw std::runtime_error(
            "REDIS_BLOCKING_POOL_SIZE_PER_WORKER is outside the supported range");
    }

    return {
        .host_ = std::string(env.get("REDIS_HOST").value_or("127.0.0.1")),
        .port_ = port,
        .username_ = std::string(env.get("REDIS_USERNAME").value_or("")),
        .password_ = std::string(env.get("REDIS_PASSWORD").value_or("")),
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .database_ = database,
        .pool_size_per_worker_ = pool_size,
        .blocking_pool_size_per_worker_ = blocking_pool_size,
        .connect_timeout_ = std::chrono::seconds(5),
        .command_timeout_ = std::chrono::seconds(5),
        .acquire_timeout_ = std::chrono::seconds(2),
    };
}

}  // namespace service::config
