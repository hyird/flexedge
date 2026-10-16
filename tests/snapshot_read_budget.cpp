#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/app.h>
#include <ruvia/web/controller.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "service/features/live_resource/read_snapshot.h"
#include "support/postgres_test.h"

namespace {

using service::live_resource::snapshot;
using service::live_resource::snapshot_read_scope;
using service::live_resource::snapshot_reads;

snapshot_reads snapshots;
std::atomic<unsigned> aggregate_queries_started{};
std::atomic<unsigned> short_follower_queries_started{};
using steady_clock = std::chrono::steady_clock;
steady_clock::time_point short_follower_leader_started_at{};
steady_clock::time_point short_follower_wait_started_at{};
std::atomic<bool> short_follower_leader_started{};
std::atomic<bool> short_follower_waiting{};
std::atomic<bool> worker_query_started{};

class snapshot_budget_controller final {
public:
    RUVIA_ROUTES_BEGIN(snapshot_budget_controller)
    RUVIA_GET("/ready", ready);
    RUVIA_GET("/aggregate", aggregate);
    RUVIA_GET("/short", short_budget);
    RUVIA_GET("/late-result", late_result);
    RUVIA_GET("/short-follower", short_follower);
    RUVIA_GET("/short-follower-metrics", short_follower_metrics);
    RUVIA_GET("/short-follower-state", short_follower_state);
    RUVIA_GET("/business-error", business_error);
    RUVIA_GET("/worker-stop", worker_stop);
    RUVIA_GET("/health-db", health_db);
    RUVIA_GET("/owner", owner);
    RUVIA_GET("/metrics", metrics);
    RUVIA_GET("/worker-started", worker_started);
    RUVIA_GET("/ticket-generation", ticket_generation);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> ready(ruvia::context& context) {
        co_return context.text(std::string_view("ready"));
    }

    ruvia::task<ruvia::http_response> aggregate(ruvia::context& context) {
        const auto key = context.req().query("key").value_or("aggregate");
        const bool fast = context.req().query("mode").value_or("leader") == "fast";
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            std::string(key), "snapshot-budget"});
        try {
            const auto result = co_await service::live_resource::read_snapshot(
                context, lease, [fast](snapshot_read_scope& scope) -> ruvia::task<std::string> {
                    auto db = scope.db();
                    if (fast) {
                        aggregate_queries_started.fetch_add(1, std::memory_order_relaxed);
                        (void)co_await db.query("SELECT 42");
                        co_return "follower-reclaimed";
                    }
                    for (int attempt = 0; attempt != 2; ++attempt) {
                        aggregate_queries_started.fetch_add(1, std::memory_order_relaxed);
                        (void)co_await db.query("SELECT pg_sleep(18)");
                    }
                    co_return "aggregate-complete";
                });
            co_return context.text(std::string_view(result->data_));
        } catch (const std::exception&) {
            co_return context.text(std::string_view("cancelled-or-deadline"));
        }
    }

    ruvia::task<ruvia::http_response> short_budget(ruvia::context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            "short", "snapshot-budget"});
        const auto deadline = snapshot_reads::clock::now() + std::chrono::milliseconds(250);
        try {
            (void)co_await service::live_resource::read_snapshot_until(
                context, lease, deadline, [](snapshot_read_scope& scope) -> ruvia::task<std::string> {
                    auto db = scope.db();
                    (void)co_await db.query("SELECT pg_sleep(3)");
                    co_return "late";
                });
            co_return context.text(std::string_view("unexpected-success"));
        } catch (const std::exception&) {
            co_return context.text(std::string_view("short-budget-cancelled"));
        }
    }

    ruvia::task<ruvia::http_response> late_result(ruvia::context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            "late-result", "snapshot-budget"});
        try {
            (void)co_await service::live_resource::read_snapshot_until(
                context, lease, snapshot_reads::clock::now() + std::chrono::milliseconds(250),
                [](snapshot_read_scope& scope) -> ruvia::task<std::string> {
                    (void)co_await ruvia::sleep_for(scope.worker(), std::chrono::milliseconds(800));
                    co_return "late-result-must-not-publish";
                });
        } catch (const std::exception&) {
            auto retry = lease.read(context.worker());
            const bool flight_reclaimed = retry.ticket_.has_value();
            if (retry.ticket_) {
                retry.ticket_->cancel();
            }
            co_return context.text(std::string_view(
                flight_reclaimed ? "late-result-discarded-reclaimed" : "late-result-was-published"));
        }
        co_return context.text(std::string_view("unexpected-success"));
    }

    ruvia::task<ruvia::http_response> short_follower(ruvia::context& context) {
        const bool leader = context.req().query("mode").value_or("follower") == "leader";
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            "short-follower", "snapshot-budget"});
        const auto budget = leader ? std::chrono::milliseconds(300)
                                   : std::chrono::milliseconds(1300);
        if (!leader) {
            short_follower_wait_started_at = steady_clock::now();
            short_follower_waiting.store(true, std::memory_order_release);
        }
        try {
            const auto result = co_await service::live_resource::read_snapshot_until(
                context, lease, snapshot_reads::clock::now() + budget,
                [leader](snapshot_read_scope& scope) -> ruvia::task<std::string> {
                    if (leader) {
                        short_follower_leader_started_at = steady_clock::now();
                        short_follower_leader_started.store(true, std::memory_order_release);
                        (void)co_await ruvia::sleep_for(scope.worker(), std::chrono::milliseconds(1000));
                        co_return "late-leader-result";
                    }
                    short_follower_queries_started.fetch_add(1, std::memory_order_relaxed);
                    auto db = scope.db();
                    (void)co_await db.query("SELECT pg_sleep(0.65)");
                    co_return "follower-result";
                });
            co_return context.text(std::string_view(result->data_));
        } catch (const std::exception&) {
            co_return context.text(std::string_view(leader ? "short-leader-cancelled"
                                                           : "short-follower-deadline"));
        }
    }

    ruvia::task<ruvia::http_response> short_follower_metrics(ruvia::context& context) {
        const auto stats = snapshots.get_stats();
        const auto body = std::to_string(short_follower_queries_started.load(std::memory_order_relaxed)) +
                          "," + std::to_string(stats.flights_);
        co_return context.text(std::string_view(body));
    }

    ruvia::task<ruvia::http_response> short_follower_state(ruvia::context& context) {
        const bool leader_started = short_follower_leader_started.load(std::memory_order_acquire);
        const bool follower_waiting = short_follower_waiting.load(std::memory_order_acquire);
        const auto delay = leader_started && follower_waiting
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                     short_follower_wait_started_at - short_follower_leader_started_at)
                                     .count()
                               : -1;
        const auto body = std::to_string(leader_started) + "," + std::to_string(follower_waiting) + "," +
                          std::to_string(short_follower_queries_started.load(std::memory_order_relaxed)) + "," +
                          std::to_string(delay);
        co_return context.text(std::string_view(body));
    }

    ruvia::task<ruvia::http_response> business_error(ruvia::context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            "business-error", "snapshot-budget"});
        const auto result = co_await service::live_resource::read_snapshot(
            context, lease, [](snapshot_read_scope&) -> ruvia::task<std::string> {
                throw std::runtime_error("expected business failure");
                co_return "unreachable";
            });
        co_return context.text(std::string_view(result->failure_ ? "business-error-cached" : "unexpected-success"));
    }

    ruvia::task<ruvia::http_response> worker_stop(ruvia::context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::resource::nodes,
            "worker-stop", "snapshot-budget"});
        try {
            (void)co_await service::live_resource::read_snapshot(
                context, lease, [](snapshot_read_scope& scope) -> ruvia::task<std::string> {
                    auto db = scope.db();
                    worker_query_started.store(true, std::memory_order_release);
                    (void)co_await db.query("SELECT pg_sleep(20)");
                    co_return "late";
                });
        } catch (...) {
        }
        co_return context.text(std::string_view("worker-stop-finished"));
    }

    ruvia::task<ruvia::http_response> health_db(ruvia::context& context) {
        const auto rows = co_await context.db().query("SELECT 42");
        co_return context.text(std::string_view(rows.size() == 1 ? "db-healthy" : "db-unhealthy"));
    }

    ruvia::task<ruvia::http_response> owner(ruvia::context& context) {
        snapshot_read_scope scope(context, snapshot_reads::clock::now() + std::chrono::seconds(2));
        const bool borrowed_owners = scope.pool() == context.pool() &&
                                     &scope.worker() == &context.worker();
        co_await scope.stop_and_join();
        co_return context.text(std::string_view(borrowed_owners ? "owner-ok" : "owner-mismatch"));
    }

    ruvia::task<ruvia::http_response> ticket_generation(ruvia::context& context) {
        snapshot_reads cache;
        auto lease = cache.subscribe({"generation-test", service::live_resource::resource::nodes,
            "same-entry", "ticket-generation"});
        auto first = lease.read(context.worker());
        if (!first.ticket_) {
            throw std::runtime_error("first ticket was not claimed");
        }
        const auto stale_ticket = *first.ticket_;
        stale_ticket.cancel();

        auto second = lease.read(context.worker());
        if (!second.ticket_) {
            throw std::runtime_error("second ticket was not claimed");
        }
        stale_ticket.complete(std::make_shared<const snapshot>(snapshot{"stale", false}));
        stale_ticket.cancel();
        if (cache.get_stats().flights_ != 1) {
            throw std::runtime_error("stale ticket consumed the new flight");
        }

        second.ticket_->complete(std::make_shared<const snapshot>(snapshot{"current", false}));
        auto received = co_await second.receiver_.receive_for(std::chrono::milliseconds(100),
            context.get_stop_token());
        if (!received.has_value()) {
            throw std::runtime_error("current ticket did not publish a result");
        }
        const auto delivered = std::move(received).take_value();
        if (!delivered.snapshot_ || delivered.snapshot_->data_ != "current" || cache.get_stats().flights_ != 0) {
            throw std::runtime_error("current ticket failed to publish exactly one result");
        }
        co_return context.text(std::string_view("ticket-generation-ok"));
    }

    ruvia::task<ruvia::http_response> worker_started(ruvia::context& context) {
        const bool started = worker_query_started.load(std::memory_order_acquire);
        co_return context.text(std::string_view(started ? "started" : "not-started"));
    }

    ruvia::task<ruvia::http_response> metrics(ruvia::context& context) {
        const auto stats = snapshots.get_stats();
        const auto body = std::to_string(aggregate_queries_started.load(std::memory_order_relaxed)) + "," +
                          std::to_string(stats.flights_);
        co_return context.text(std::string_view(body));
    }
};


}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("expected loopback HTTP port");
        }
        const auto base = test_support::isolated_postgres_config();
        if (!base) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*base);
        auto& app = ruvia::app();
        // Mirrors service/server.cpp: stop() closes in-flight requests at once.
        app.server({.worker_count_ = 2,
                       .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
                       .drain_timeout_ = std::chrono::milliseconds::zero()})
            .database({.alias_ = "default", .config_ = temporary_database.config()})
            .listen({.address_ = "127.0.0.1", .http_ = static_cast<std::uint16_t>(std::stoul(argv[1]))});
        app.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "snapshot budget fixture failed: " << error.what() << '\n';
        return 1;
    }
}
