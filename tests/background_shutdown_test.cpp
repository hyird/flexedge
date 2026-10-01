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

ruvia::DbConfig isolatedPostgresConfig() {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database)
        throw std::runtime_error("PostgreSQL QA environment is unavailable");
    if (std::string(host) != "127.0.0.1" || !std::string(database).ends_with("_auth_qa"))
        throw std::runtime_error("background shutdown test requires isolated loopback _auth_qa DB");
    const auto* port = std::getenv("PGPORT");
    const auto* user = std::getenv("PGUSER");
    const auto* password = std::getenv("PGPASSWORD");
    return {
        .driver = ruvia::DbDriver::kPostgreSql,
        .host = host,
        .port = static_cast<std::uint16_t>(port ? std::stoi(port) : 5432),
        .username = user ? user : "auth_qa",
        .password = password ? password : "qa-only",
        .database = database,
    };
}

} // namespace

int main() {
    const auto* host = std::getenv("PGHOST");
    const auto* database = std::getenv("PGDATABASE");
    if (!host || !database)
        return 77;

    try {
        auto db = isolatedPostgresConfig();
        auto origins = service::config::makeOutboundOrigins("");
        std::atomic_bool queryStarted{};
        std::atomic_bool workerFinished{};
        std::vector<service::background::WorkerDefinition> definitions;
        definitions.push_back({
            .name = "background-shutdown-test",
            .outboundAliases = {},
            .run = [&queryStarted, &workerFinished](service::background::WorkerContext& context)
                -> ruvia::Task<void> {
                queryStarted.store(true, std::memory_order_release);
                try {
                    (void)co_await context.db().query("SELECT pg_sleep(30)");
                } catch (...) {
                    // Shutdown cancels the worker-owned DB operation; reaching this line
                    // and returning proves the business coroutine completed before join.
                }
                workerFinished.store(true, std::memory_order_release);
            },
        });
        service::background::WorkerPool pool(db, origins, std::move(definitions));
        pool.start();

        const auto startedDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!queryStarted.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < startedDeadline)
            std::this_thread::yield();
        if (!queryStarted.load(std::memory_order_acquire)) {
            pool.stop();
            throw std::runtime_error("background DB query did not start");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pool.stop();
        if (!workerFinished.load(std::memory_order_acquire))
            throw std::runtime_error("WorkerPool returned before business DB task completed");
        std::cout << "background DB shutdown joined in-flight query and worker cleanup\n";

        auto unavailableDb = db;
        unavailableDb.database += "_missing";
        std::vector<service::background::WorkerDefinition> unavailableWorkers;
        unavailableWorkers.push_back({
            .name = "background-startup-failure-test",
            .outboundAliases = {},
            .run = [](service::background::WorkerContext&) -> ruvia::Task<void> { co_return; },
        });
        service::background::WorkerPool unavailablePool(
            unavailableDb, origins, std::move(unavailableWorkers));
        bool connectionFailureObserved{};
        try {
            unavailablePool.start();
        } catch (const std::exception&) {
            connectionFailureObserved = true;
        }
        if (!connectionFailureObserved)
            throw std::runtime_error("WorkerPool startup unexpectedly accepted missing QA database");
        std::cout << "background startup failure cleaned up DB connection tasks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
