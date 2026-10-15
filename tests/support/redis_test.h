#pragma once

#include <ruvia/web/redis/redis_types.h>

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace test_support {

// The loopback isolated Redis database named by REDIS_TEST_PORT and
// REDIS_TEST_DATABASE (e.g. the dev container on 127.0.0.1:16379, DB 9).
// Nothing when they are unset (the test is skipped). Refuses the default
// Redis port and database 0, so a test never touches a shared instance.
inline std::optional<ruvia::redis_config> isolated_redis_config() {
    const auto* port = std::getenv("REDIS_TEST_PORT");
    const auto* database = std::getenv("REDIS_TEST_DATABASE");
    if (!port || !database) {
        return std::nullopt;
    }
    const auto port_value = std::stoul(port);
    const auto database_value = std::stoul(database);
    if (port_value == 0 || port_value > 65535 || port_value == 6379 || database_value == 0) {
        throw std::runtime_error(
            "Requires an isolated loopback Redis: REDIS_TEST_PORT other than 6379 and "
            "REDIS_TEST_DATABASE other than 0");
    }
    ruvia::redis_config config;
    config.host_ = "127.0.0.1";
    config.port_ = static_cast<std::uint16_t>(port_value);
    config.database_ = static_cast<std::uint32_t>(database_value);
    config.pool_size_per_worker_ = 1;
    config.tls_ = {.mode_ = ruvia::client_tls_mode::disabled};
    config.blocking_pool_size_per_worker_ = 1;
    if (const auto* password = std::getenv("REDIS_TEST_PASSWORD")) {
        config.password_ = password;
    }
    return config;
}

}  // namespace test_support
