#pragma once
#include <array>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>
#include <ruvia/core/AsioTask.h>
#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/db/DbClient.h>


namespace test_support {
template <typename Verify>
int runPostgresTest(Verify verify) {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database) return 77;
    if (std::string(host) != "127.0.0.1" || !std::string(database).ends_with("_auth_qa")) {
        std::cerr << "Requires loopback isolated PostgreSQL database ending in _auth_qa\n";
        return 1;
    }
    const auto* port = std::getenv("PGPORT");
    const auto* user = std::getenv("PGUSER");
    const auto* password = std::getenv("PGPASSWORD");
    ruvia::EventLoopPool loops({.loopCount = 1});
    const ruvia::DbConfig config{
        .driver = ruvia::DbDriver::kPostgreSql,
        .host = host,
        .port = static_cast<std::uint16_t>(port ? std::stoi(port) : 5432),
        .username = user ? user : "auth_qa",
        .password = password ? password : "qa-only",
        .database = database,
    };
    ruvia::DbClient client(loops.loop(0), config);
    std::optional<ruvia::DbClient> peer;
    if constexpr (std::is_invocable_v<Verify, ruvia::DbClient&, ruvia::DbClient&>) {
        peer.emplace(loops.loop(0), config);
    }
    loops.start();
    int result = 0;
    try {
        auto verification = [&] {
            if constexpr (std::is_invocable_v<Verify, ruvia::DbClient&, ruvia::DbClient&>) {
                return verify(client, *peer);
            } else {
                return verify(client);
            }
        };
        asio::co_spawn(loops.loop(0).executor(), ruvia::asAwaitable(verification()),
                       asio::use_future).get();
        std::cout << "PostgreSQL storage test passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    asio::co_spawn(loops.loop(0).executor(), ruvia::asAwaitable(client.shutdown()),
                   asio::use_future).get();
    if (peer) {
        asio::co_spawn(loops.loop(0).executor(), ruvia::asAwaitable(peer->shutdown()),
                       asio::use_future).get();
    }
    loops.stop();
    loops.join();
    return result;
}

} // namespace test_support
