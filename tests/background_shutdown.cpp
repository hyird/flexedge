#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/app.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "service/features/background/task_context.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

using service::background::task_context;

struct test_observations final {
    std::atomic_uint started_{};
    std::atomic_uint completed_{};
    std::atomic_uint joined_{};
    std::atomic_uint failures_{};
};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ruvia::task<void> wait_for_database(task_context& context, test_observations& observations) {
    observations.started_.fetch_add(1, std::memory_order_release);
    bool cancelled{};
    try {
        (void)co_await context.db().with_options({.timeout_ = std::chrono::seconds(31)}).query("SELECT pg_sleep(30)");
    } catch (const std::exception&) {
        cancelled = context.get_stop_token().stop_requested();
    }
    if (!cancelled || !context.worker().is_current()) {
        observations.failures_.fetch_add(1, std::memory_order_release);
    }
    observations.completed_.fetch_add(1, std::memory_order_release);
}

ruvia::task<void> wait_for_redis(task_context& context, test_observations& observations) {
    observations.started_.fetch_add(1, std::memory_order_release);
    bool cancelled{};
    try {
        const auto key = "flexedge:test:background-shutdown:" + std::string(context.lease_owner());
        (void)co_await context.redis().with_options({.timeout_ = std::chrono::seconds(31)}).command("XREAD", "BLOCK", "30000", "STREAMS", key, "0-0");
    } catch (const std::exception&) {
        cancelled = context.get_stop_token().stop_requested();
    }
    if (!cancelled || !context.worker().is_current()) {
        observations.failures_.fetch_add(1, std::memory_order_release);
    }
    observations.completed_.fetch_add(1, std::memory_order_release);
}

ruvia::task<void> verify_web_tasks(ruvia::web_worker_context& web, test_observations& observations) {
    // Every Web Worker executes the same suite. Contexts have distinct consumer
    // identities even when their tasks execute concurrently on the same worker.
    ruvia::task_scope local_tasks(web.worker(), {.resource_ = web.pool()});
    task_context local_database(web, local_tasks.get_stop_token());
    task_context local_redis(web, local_tasks.get_stop_token());
    require(&local_database.worker() == &web.worker() && local_database.pool() == web.pool(),
        "task context must borrow its Web Worker's handle and pool");
    require(local_database.lease_owner() != local_redis.lease_owner(),
        "concurrent task invocations must have unique consumer names");
    require(local_database.get_http_client("shutdown-test").port() == 1,
        "task HTTP client must use the Web Worker's registered origin");
    (void)co_await local_redis.redis().ping();
    local_tasks.spawn(wait_for_database(local_database, observations));
    local_tasks.spawn(wait_for_redis(local_redis, observations));
    (void)co_await ruvia::sleep_for(web.worker(), std::chrono::milliseconds(100), web.get_stop_token());
    local_tasks.request_stop();
    co_await local_tasks.join();

    // Cancelling a task scope must leave the parent worker's clients usable.
    task_context database(web);
    task_context redis(web);
    require(database.lease_owner() != local_database.lease_owner(), "a later invocation needs a fresh lease owner");
    (void)co_await database.db().query("SELECT 1");
    (void)co_await redis.redis().ping();
    ruvia::task_scope server_tasks(web.worker(), {.resource_ = web.pool()});
    server_tasks.spawn(wait_for_database(database, observations));
    server_tasks.spawn(wait_for_redis(redis, observations));
    co_await server_tasks.join();
    observations.joined_.fetch_add(1, std::memory_order_release);
}

std::uint16_t available_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

}  // namespace

int main() {
    try {
        const auto database = test_support::isolated_postgres_config();
        const auto redis = test_support::isolated_redis_config();
        if (!database || !redis) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*database);
        auto& app = ruvia::app();
        app.database({.config_ = temporary_database.config()});
        app.redis({.config_ = *redis});
        app.get_http_client({.alias_ = "shutdown-test", .config_ = {.host_ = "127.0.0.1", .port_ = 1}});
        app.server({.worker_count_ = 2, .drain_timeout_ = std::chrono::milliseconds(0)});
        app.listen({.address_ = "127.0.0.1", .http_ = available_port()});
        test_observations observations;
        app.on_start([&] {
            const auto workers = app.workers();
            require(workers.size() == 2, "shutdown test requires two identical Web Workers");
            for (const auto& worker : workers) {
                const auto posted = worker.post([&](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    co_await verify_web_tasks(context, observations);
                });
                require(posted.accepted(), "Web Worker task was not accepted");
            }
        });
        std::atomic_bool timed_out{};
        std::jthread shutdown([&](std::stop_token stop) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (!stop.stop_requested() && observations.started_.load(std::memory_order_acquire) != 8 &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (stop.stop_requested()) {
                return;
            }
            timed_out.store(observations.started_.load(std::memory_order_acquire) != 8, std::memory_order_release);
            // Let both real PostgreSQL sleeps and Redis BLOCK reads become
            // in-flight before asking the server to retire its Web Workers.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            app.stop();
        });
        app.run();
        shutdown.request_stop();
        shutdown.join();
        require(!timed_out.load(std::memory_order_acquire), "Web Worker operations did not start");
        require(observations.failures_.load(std::memory_order_acquire) == 0,
            "task cancellation failed or resumed on another worker");
        require(observations.completed_.load(std::memory_order_acquire) == 8 &&
                    observations.joined_.load(std::memory_order_acquire) == 2,
            "server returned before Web Worker tasks and their cleanup completed");
        std::cout << "identical Web Workers joined PostgreSQL and Redis operations after task cancellation and server shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
