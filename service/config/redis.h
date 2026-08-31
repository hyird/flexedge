#pragma once

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <ruvia/web/Dotenv.h>
#include <ruvia/web/redis/Redis.h>

namespace service::config {

inline ruvia::RedisConfig redisConfig(const ruvia::Env& env) {
    const auto port = env.get<std::uint16_t>("REDIS_PORT").value_or(6379);
    if (port == 0) {
        throw std::runtime_error("REDIS_PORT must be a positive TCP port");
    }
    const auto database = env.get<std::uint32_t>("REDIS_DATABASE").value_or(0);
    const auto poolSize = env.get<std::size_t>("REDIS_POOL_SIZE_PER_WORKER").value_or(2);
    if (poolSize == 0 || poolSize > 256) {
        throw std::runtime_error("REDIS_POOL_SIZE_PER_WORKER is outside the supported range");
    }
    const auto blockingPoolSize =
        env.get<std::size_t>("REDIS_BLOCKING_POOL_SIZE_PER_WORKER").value_or(16);
    if (blockingPoolSize == 0 || blockingPoolSize > 256) {
        throw std::runtime_error(
            "REDIS_BLOCKING_POOL_SIZE_PER_WORKER is outside the supported range");
    }

    return {
        .host = std::string(env.get("REDIS_HOST").value_or("127.0.0.1")),
        .port = port,
        .username = std::string(env.get("REDIS_USERNAME").value_or("")),
        .password = std::string(env.get("REDIS_PASSWORD").value_or("")),
        .database = database,
        .poolSizePerWorker = poolSize,
        .blockingPoolSizePerWorker = blockingPoolSize,
        .connectTimeout = std::chrono::seconds(5),
        .commandTimeout = std::chrono::seconds(5),
        .acquireTimeout = std::chrono::seconds(2),
    };
}

} // namespace service::config
