#pragma once
#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_types.h>

#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

namespace test_support {
// The loopback isolated database named by PG* variables. Nothing when
// PGHOST/PGDATABASE are unset (the test is skipped); throws when they name
// anything but a loopback database ending in _auth_qa.
inline std::optional<ruvia::db_config> isolated_postgres_config() {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database) {
        return std::nullopt;
    }
    if (std::string(host) != "127.0.0.1" || !std::string(database).ends_with("_auth_qa")) {
        throw std::runtime_error("Requires loopback isolated PostgreSQL database ending in _auth_qa");
    }
    const auto* port = std::getenv("PGPORT");
    const auto* user = std::getenv("PGUSER");
    const auto* password = std::getenv("PGPASSWORD");
    ruvia::db_config config;
    config.driver_ = ruvia::db_driver::postgresql;
    config.host_ = host;
    config.port_ = static_cast<std::uint16_t>(port ? std::stoi(port) : 5432);
    config.username_ = user ? user : "auth_qa";
    config.password_ = password ? password : "qa-only";
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.database_ = database;
    return config;
}

// Runs `verify(client)` or `verify(client, peer)` against `config` on one loop.
template <typename verify_type>
int run_postgres_test(const ruvia::db_config& config, verify_type verify) {
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    ruvia::db_client client(loops.loop(0), config);
    std::optional<ruvia::db_client> peer;
    if constexpr (std::is_invocable_v<verify_type, ruvia::db_client&, ruvia::db_client&>) {
        peer.emplace(loops.loop(0), config);
    }
    loops.start();
    int result = 0;
    try {
        auto verification = [&] {
            if constexpr (std::is_invocable_v<verify_type, ruvia::db_client&, ruvia::db_client&>) {
                return verify(client, *peer);
            } else {
                return verify(client);
            }
        };
        asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(verification()),
            asio::use_future)
            .get();
        std::cout << "PostgreSQL storage test passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(client.shutdown()),
        asio::use_future)
        .get();
    if (peer) {
        asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(peer->shutdown()),
            asio::use_future)
            .get();
    }
    loops.stop();
    loops.join();
    return result;
}

template <typename verify_type>
int run_postgres_test(verify_type verify) {
    std::optional<ruvia::db_config> config;
    try {
        config = isolated_postgres_config();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (!config) {
        return 77;
    }
    return run_postgres_test(*config, std::move(verify));
}

}  // namespace test_support
