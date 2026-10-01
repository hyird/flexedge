#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Deadline.h>

#include "service/features/live_resource/read_snapshot.h"

namespace {

using service::live_resource::Snapshot;
using service::live_resource::SnapshotCache;
using service::live_resource::SnapshotReadScope;

SnapshotCache snapshots;
std::atomic<unsigned> aggregateQueriesStarted{};
std::atomic<unsigned> shortFollowerQueriesStarted{};
using SteadyClock = std::chrono::steady_clock;
SteadyClock::time_point shortFollowerLeaderStartedAt{};
SteadyClock::time_point shortFollowerWaitStartedAt{};
std::atomic<bool> shortFollowerLeaderStarted{};
std::atomic<bool> shortFollowerWaiting{};
std::atomic<bool> workerQueryStarted{};

class SnapshotBudgetController final : public ruvia::Controller<SnapshotBudgetController> {
public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/ready", ready);
    RUVIA_GET("/aggregate", aggregate);
    RUVIA_GET("/short", shortBudget);
    RUVIA_GET("/late-result", lateResult);
    RUVIA_GET("/short-follower", shortFollower);
    RUVIA_GET("/short-follower-metrics", shortFollowerMetrics);
    RUVIA_GET("/short-follower-state", shortFollowerState);
    RUVIA_GET("/business-error", businessError);
    RUVIA_GET("/context-stop", contextStop, ruvia::Deadline<250>);
    RUVIA_GET("/worker-stop", workerStop);
    RUVIA_GET("/health-db", healthDb);
    RUVIA_GET("/owner", owner);
    RUVIA_GET("/metrics", metrics);
    RUVIA_GET("/worker-started", workerStarted);
    RUVIA_GET("/ticket-generation", ticketGeneration);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> ready(ruvia::Context& context) {
        co_return context.text(std::string_view("ready"));
    }

    ruvia::Task<ruvia::HttpResponse> aggregate(ruvia::Context& context) {
        const auto key = context.req().query("key").value_or("aggregate");
        const bool fast = context.req().query("mode").value_or("leader") == "fast";
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          std::string(key), "snapshot-budget"});
        try {
            const auto result = co_await service::live_resource::readSnapshot(
                context, lease, [fast](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    auto db = scope.db();
                    if (fast) {
                        aggregateQueriesStarted.fetch_add(1, std::memory_order_relaxed);
                        (void)co_await db.query("SELECT 42");
                        co_return "follower-reclaimed";
                    }
                    for (int attempt = 0; attempt != 2; ++attempt) {
                        aggregateQueriesStarted.fetch_add(1, std::memory_order_relaxed);
                        (void)co_await db.query("SELECT pg_sleep(18)");
                    }
                    co_return "aggregate-complete";
                });
            co_return context.text(std::string_view(result->data));
        } catch (const std::exception&) {
            co_return context.text(std::string_view("cancelled-or-deadline"));
        }
    }

    ruvia::Task<ruvia::HttpResponse> shortBudget(ruvia::Context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "short", "snapshot-budget"});
        const auto deadline = SnapshotCache::Clock::now() + std::chrono::milliseconds(250);
        try {
            (void)co_await service::live_resource::readSnapshotUntil(
                context, lease, deadline, [](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    auto db = scope.db();
                    (void)co_await db.query("SELECT pg_sleep(3)");
                    co_return "late";
                });
            co_return context.text(std::string_view("unexpected-success"));
        } catch (const std::exception&) {
            co_return context.text(std::string_view("short-budget-cancelled"));
        }
    }

    ruvia::Task<ruvia::HttpResponse> lateResult(ruvia::Context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "late-result", "snapshot-budget"});
        try {
            (void)co_await service::live_resource::readSnapshotUntil(
                context, lease, SnapshotCache::Clock::now() + std::chrono::milliseconds(250),
                [](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    (void)co_await ruvia::sleepFor(scope.worker(), std::chrono::milliseconds(800));
                    co_return "late-result-must-not-publish";
                });
        } catch (const std::exception&) {
            auto retry = lease.read(context.worker());
            const bool flightReclaimed = retry.ticket.has_value();
            if (retry.ticket) retry.ticket->cancel();
            co_return context.text(std::string_view(
                flightReclaimed ? "late-result-discarded-reclaimed" : "late-result-was-published"));
        }
        co_return context.text(std::string_view("unexpected-success"));
    }

    ruvia::Task<ruvia::HttpResponse> shortFollower(ruvia::Context& context) {
        const bool leader = context.req().query("mode").value_or("follower") == "leader";
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "short-follower", "snapshot-budget"});
        const auto budget = leader ? std::chrono::milliseconds(300)
                                   : std::chrono::milliseconds(1300);
        if (!leader) {
            shortFollowerWaitStartedAt = SteadyClock::now();
            shortFollowerWaiting.store(true, std::memory_order_release);
        }
        try {
            const auto result = co_await service::live_resource::readSnapshotUntil(
                context, lease, SnapshotCache::Clock::now() + budget,
                [leader](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    if (leader) {
                        shortFollowerLeaderStartedAt = SteadyClock::now();
                        shortFollowerLeaderStarted.store(true, std::memory_order_release);
                        (void)co_await ruvia::sleepFor(scope.worker(), std::chrono::milliseconds(1000));
                        co_return "late-leader-result";
                    }
                    shortFollowerQueriesStarted.fetch_add(1, std::memory_order_relaxed);
                    auto db = scope.db();
                    (void)co_await db.query("SELECT pg_sleep(0.65)");
                    co_return "follower-result";
                });
            co_return context.text(std::string_view(result->data));
        } catch (const std::exception&) {
            co_return context.text(std::string_view(leader ? "short-leader-cancelled"
                                                           : "short-follower-deadline"));
        }
    }

    ruvia::Task<ruvia::HttpResponse> shortFollowerMetrics(ruvia::Context& context) {
        const auto stats = snapshots.stats();
        const auto body = std::to_string(shortFollowerQueriesStarted.load(std::memory_order_relaxed)) +
                          "," + std::to_string(stats.flights);
        co_return context.text(std::string_view(body));
    }

    ruvia::Task<ruvia::HttpResponse> shortFollowerState(ruvia::Context& context) {
        const bool leaderStarted = shortFollowerLeaderStarted.load(std::memory_order_acquire);
        const bool followerWaiting = shortFollowerWaiting.load(std::memory_order_acquire);
        const auto delay = leaderStarted && followerWaiting
                               ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                     shortFollowerWaitStartedAt - shortFollowerLeaderStartedAt)
                                     .count()
                               : -1;
        const auto body = std::to_string(leaderStarted) + "," + std::to_string(followerWaiting) + "," +
                          std::to_string(shortFollowerQueriesStarted.load(std::memory_order_relaxed)) + "," +
                          std::to_string(delay);
        co_return context.text(std::string_view(body));
    }

    ruvia::Task<ruvia::HttpResponse> businessError(ruvia::Context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "business-error", "snapshot-budget"});
        const auto result = co_await service::live_resource::readSnapshot(
            context, lease, [](SnapshotReadScope&) -> ruvia::Task<std::string> {
                throw std::runtime_error("expected business failure");
                co_return "unreachable";
            });
        co_return context.text(std::string_view(result->failure ? "business-error-cached" : "unexpected-success"));
    }

    ruvia::Task<ruvia::HttpResponse> contextStop(ruvia::Context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "context-stop", "snapshot-budget"});
        try {
            (void)co_await service::live_resource::readSnapshot(
                context, lease, [](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    auto db = scope.db();
                    (void)co_await db.query("SELECT pg_sleep(3)");
                    co_return "late";
                });
            co_return context.text("unexpected-success");
        } catch (const std::exception&) {
            co_return context.text(std::string_view("context-stop-cancelled"));
        }
    }

    ruvia::Task<ruvia::HttpResponse> workerStop(ruvia::Context& context) {
        auto lease = snapshots.subscribe({"test-tenant", service::live_resource::Resource::nodes,
                                          "worker-stop", "snapshot-budget"});
        try {
            (void)co_await service::live_resource::readSnapshot(
                context, lease, [](SnapshotReadScope& scope) -> ruvia::Task<std::string> {
                    auto db = scope.db();
                    workerQueryStarted.store(true, std::memory_order_release);
                    (void)co_await db.query("SELECT pg_sleep(20)");
                    co_return "late";
                });
        } catch (...) {
        }
        co_return context.text(std::string_view("worker-stop-finished"));
    }

    ruvia::Task<ruvia::HttpResponse> healthDb(ruvia::Context& context) {
        const auto rows = co_await context.db().query("SELECT 42");
        co_return context.text(std::string_view(rows.size() == 1 ? "db-healthy" : "db-unhealthy"));
    }

    ruvia::Task<ruvia::HttpResponse> owner(ruvia::Context& context) {
        SnapshotReadScope scope(context, SnapshotCache::Clock::now() + std::chrono::seconds(2));
        const bool borrowedOwners = scope.pool() == context.pool() &&
                                    &scope.worker() == &context.worker();
        co_await scope.stopAndJoin();
        co_return context.text(std::string_view(borrowedOwners ? "owner-ok" : "owner-mismatch"));
    }

    ruvia::Task<ruvia::HttpResponse> ticketGeneration(ruvia::Context& context) {
        SnapshotCache cache;
        auto lease = cache.subscribe({"generation-test", service::live_resource::Resource::nodes,
                                      "same-entry", "ticket-generation"});
        auto first = lease.read(context.worker());
        if (!first.ticket) throw std::runtime_error("first ticket was not claimed");
        const auto staleTicket = *first.ticket;
        staleTicket.cancel();

        auto second = lease.read(context.worker());
        if (!second.ticket) throw std::runtime_error("second ticket was not claimed");
        staleTicket.complete(std::make_shared<const Snapshot>(Snapshot{"stale", false}));
        staleTicket.cancel();
        if (cache.stats().flights != 1)
            throw std::runtime_error("stale ticket consumed the new flight");

        second.ticket->complete(std::make_shared<const Snapshot>(Snapshot{"current", false}));
        auto received = co_await second.receiver.receiveFor(std::chrono::milliseconds(100),
                                                             context.stopToken());
        if (!received.hasValue())
            throw std::runtime_error("current ticket did not publish a result");
        const auto delivered = std::move(received).takeValue();
        if (!delivered.snapshot || delivered.snapshot->data != "current" || cache.stats().flights != 0)
            throw std::runtime_error("current ticket failed to publish exactly one result");
        co_return context.text(std::string_view("ticket-generation-ok"));
    }

    ruvia::Task<ruvia::HttpResponse> workerStarted(ruvia::Context& context) {
        const bool started = workerQueryStarted.load(std::memory_order_acquire);
        co_return context.text(std::string_view(started ? "started" : "not-started"));
    }

    ruvia::Task<ruvia::HttpResponse> metrics(ruvia::Context& context) {
        const auto stats = snapshots.stats();
        const auto body = std::to_string(aggregateQueriesStarted.load(std::memory_order_relaxed)) + "," +
                          std::to_string(stats.flights);
        co_return context.text(std::string_view(body));
    }
};

std::string requiredEnvironment(const char* key) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') throw std::runtime_error(std::string("missing ") + key);
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected loopback HTTP port");
        const auto host = requiredEnvironment("PGHOST");
        const auto database = requiredEnvironment("PGDATABASE");
        if (host != "127.0.0.1" || !database.ends_with("_auth_qa"))
            throw std::runtime_error("refusing PostgreSQL target outside isolated loopback *_auth_qa");
        const auto portText = requiredEnvironment("PGPORT");
        const auto port = static_cast<std::uint16_t>(std::stoul(portText));
        const auto username = requiredEnvironment("PGUSER");
        const auto password = requiredEnvironment("PGPASSWORD");

        ruvia::DbConfig db{
            .driver = ruvia::DbDriver::kPostgreSql,
            .host = host,
            .port = port,
            .username = username,
            .password = password,
            .database = database,
        };
        auto& app = ruvia::app();
        app.server({.workerCount = 2, .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall})
            .database({.alias = "default", .config = std::move(db)})
            .listen({.address = "127.0.0.1", .http = static_cast<std::uint16_t>(std::stoul(argv[1]))});
        app.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "snapshot budget fixture failed: " << error.what() << '\n';
        return 1;
    }
}
