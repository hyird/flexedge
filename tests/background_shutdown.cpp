#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "service/config/outbound.h"
#include "service/features/background/worker_pool.h"

namespace {

ruvia::db_config isolated_postgres_config() {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database) {
        throw std::runtime_error("PostgreSQL QA environment is unavailable");
    }
    if (std::string(host) != "127.0.0.1" || !std::string(database).ends_with("_auth_qa")) {
        throw std::runtime_error("background shutdown test requires isolated loopback _auth_qa DB");
    }
    const auto* port = std::getenv("PGPORT");
    const auto* user = std::getenv("PGUSER");
    const auto* password = std::getenv("PGPASSWORD");
    return {
        .driver_ = ruvia::db_driver::postgresql,
        .host_ = host,
        .port_ = static_cast<std::uint16_t>(port ? std::stoi(port) : 5432),
        .username_ = user ? user : "auth_qa",
        .password_ = password ? password : "qa-only",
        .tls_ = {.mode_ = ruvia::client_tls_mode::disabled},
        .database_ = database,
    };
}

}  // namespace

int main() {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database) {
        return 77;
    }

    try {
        auto db = isolated_postgres_config();
        auto origins = service::config::make_outbound_origins("");
        std::atomic_bool query_started{};
        std::atomic_bool worker_finished{};
        std::vector<service::background::worker_definition> definitions;
        definitions.push_back({
            .name_ = "background-shutdown-test",
            .outbound_aliases_ = {},
            .run_ = [&query_started, &worker_finished](service::background::worker_context& context)
                -> ruvia::task<void> {
                query_started.store(true, std::memory_order_release);
                try {
                    (void)co_await context.db().query("SELECT pg_sleep(30)");
                } catch (...) {
                    // Shutdown cancels the worker-owned DB operation; reaching this line
                    // and returning proves the business coroutine completed before join.
                }
                worker_finished.store(true, std::memory_order_release);
            },
        });
        service::background::worker_pool pool(db, origins, std::move(definitions));
        pool.start();

        const auto started_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!query_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < started_deadline) {
            std::this_thread::yield();
        }
        if (!query_started.load(std::memory_order_acquire)) {
            pool.stop();
            throw std::runtime_error("background DB query did not start");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pool.stop();
        if (!worker_finished.load(std::memory_order_acquire)) {
            throw std::runtime_error("WorkerPool returned before business DB task completed");
        }
        std::cout << "background DB shutdown joined in-flight query and worker cleanup\n";

        auto unavailable_db = db;
        unavailable_db.database_ += "_missing";
        std::vector<service::background::worker_definition> unavailable_workers;
        unavailable_workers.push_back({
            .name_ = "background-startup-failure-test",
            .outbound_aliases_ = {},
            .run_ = [](service::background::worker_context&) -> ruvia::task<void> { co_return; },
        });
        service::background::worker_pool unavailable_pool(
            unavailable_db, origins, std::move(unavailable_workers));
        bool connection_failure_observed{};
        try {
            unavailable_pool.start();
        } catch (const std::exception&) {
            connection_failure_observed = true;
        }
        if (!connection_failure_observed) {
            throw std::runtime_error("WorkerPool startup unexpectedly accepted missing QA database");
        }
        std::cout << "background startup failure cleaned up DB connection tasks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
