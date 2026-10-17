#pragma once
#include <ruvia/core/asio_task.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_types.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

#include "tests/support/environment.h"

namespace test_support {
// PG* names only the administrative connection, never a test's working database.
// Missing configuration skips the test; unsafe targets fail before any DDL.
inline std::optional<ruvia::db_config> isolated_postgres_config() {
    const auto host = environment_variable("PGHOST");
    const auto database = environment_variable("PGDATABASE");
    if (!host || !database) {
        return std::nullopt;
    }
    if (*host != "127.0.0.1" || !database->ends_with("_auth_qa")) {
        throw std::runtime_error("Requires loopback isolated PostgreSQL database ending in _auth_qa");
    }
    const auto port = environment_variable("PGPORT");
    const auto user = environment_variable("PGUSER");
    const auto password = environment_variable("PGPASSWORD");
    ruvia::db_config config;
    config.driver_ = ruvia::db_driver::postgresql;
    config.host_ = *host;
    config.port_ = static_cast<std::uint16_t>(port ? std::stoi(*port) : 5432);
    config.username_ = user.value_or("auth_qa");
    config.password_ = password.value_or("qa-only");
    config.tls_.mode_ = ruvia::client_tls_mode::disabled;
    config.database_ = *database;
    return config;
}

// Owns an empty, randomly named database. Migration tests keep several owners
// alive across old/new migrators; ordinary tests install only their own fixtures.
// Declare before test clients/apps so their normal shutdown precedes FORCE drop.
class postgres_test_database final {
public:
    explicit postgres_test_database(const ruvia::db_config& base)
        : base_(base),
          config_(base) {
        if (base.host_ != "127.0.0.1" || !base.database_.ends_with("_auth_qa")) {
            throw std::runtime_error("Requires loopback isolated PostgreSQL database ending in _auth_qa");
        }
        std::random_device random;
        config_.database_ = "flexedge_";
        constexpr char digits[] = "0123456789abcdef";
        for (int index = 0; index < 32; ++index) {
            config_.database_ += digits[random() & 15];
        }
        config_.database_ += "_auth_qa";
        // Only generated lowercase hexadecimal enters DDL, always as an identifier.
        bool created = false;
        try {
            execute("CREATE DATABASE \"" + config_.database_ + "\"", &created);
        } catch (...) {
            if (created) {
                execute("DROP DATABASE \"" + config_.database_ + "\" WITH (FORCE)");
            }
            throw;
        }
    }

    postgres_test_database(const postgres_test_database&) = delete;
    postgres_test_database& operator=(const postgres_test_database&) = delete;

    ~postgres_test_database() noexcept {
        try {
            execute("DROP DATABASE \"" + config_.database_ + "\" WITH (FORCE)");
        } catch (const std::exception& error) {
            std::cerr << "Temporary PostgreSQL database cleanup failed: " << error.what() << '\n';
            std::terminate();
        }
    }

    const ruvia::db_config& config() const noexcept {
        return config_;
    }

    // Runs verify(client) or verify(client, peer) without replacing this database.
    template <typename verify_type>
    int run(verify_type verify) const {
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        ruvia::db_client client(loops.loop(0), config_);
        std::optional<ruvia::db_client> peer;
        if constexpr (std::is_invocable_v<verify_type, ruvia::db_client&, ruvia::db_client&>) {
            peer.emplace(loops.loop(0), config_);
        }
        loops.start();
        std::exception_ptr failure;
        try {
            auto verification = [&] {
                if constexpr (std::is_invocable_v<verify_type, ruvia::db_client&, ruvia::db_client&>) {
                    return verify(client, *peer);
                } else {
                    return verify(client);
                }
            };
            asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(verification()), asio::use_future).get();
        } catch (...) {
            failure = std::current_exception();
        }
        auto shutdown = [&](ruvia::db_client& connection) {
            try {
                asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(connection.shutdown()), asio::use_future).get();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        };
        shutdown(client);
        if (peer) {
            shutdown(*peer);
        }
        loops.stop();
        loops.join();
        if (failure) {
            try {
                std::rethrow_exception(failure);
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
            } catch (...) {
                std::cerr << "PostgreSQL storage test failed with a non-standard exception\n";
            }
            return 1;
        }
        std::cout << "PostgreSQL storage test passed\n";
        return 0;
    }

private:
    ruvia::task<void> execute_on(ruvia::db_client& client, const std::string& sql, bool* completed) const {
        std::exception_ptr failure;
        try {
            co_await client.connect();
            (void)co_await client.handle().execute(sql);
            if (completed) {
                *completed = true;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            co_await client.shutdown();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void execute(const std::string& sql, bool* completed = nullptr) const {
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        ruvia::db_client client(loops.loop(0), base_);
        loops.start();
        std::exception_ptr failure;
        try {
            asio::co_spawn(loops.loop(0).executor(), ruvia::as_awaitable(execute_on(client, sql, completed)), asio::use_future).get();
        } catch (...) {
            failure = std::current_exception();
        }
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::db_config base_;
    ruvia::db_config config_;
};

template <typename verify_type>
int run_postgres_test(verify_type verify) {
    try {
        const auto base = isolated_postgres_config();
        if (!base) {
            return 77;
        }
        const postgres_test_database database(*base);
        return database.run(std::move(verify));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

}  // namespace test_support
