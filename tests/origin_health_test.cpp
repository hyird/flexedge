#include <atomic>
#include <chrono>
#include <latch>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#include "node/data/origin_health.h"

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error("requirement failed: " #condition); } while (false)

static_assert(!std::is_default_constructible_v<flexedge::node::OriginHealthRegistry::PreparedStates>);

int main() {
    flexedge::node::OriginHealthRegistry healthState;
    healthState.publish(healthState.prepare({flexedge::node::OriginHealthRegistry::key("website", "origin")}));
    REQUIRE(healthState.healthy("website", "origin"));
    healthState.target("website", "origin").failure(2);
    REQUIRE(healthState.healthy("website", "origin"));
    healthState.target("website", "origin").failure(2);
    REQUIRE(!healthState.healthy("website", "origin"));
    healthState.target("website", "origin").success(2);
    REQUIRE(!healthState.healthy("website", "origin"));
    healthState.target("website", "origin").success(2);
    REQUIRE(healthState.healthy("website", "origin"));
    flexedge::node::OriginHealthRegistry::KeySet retainedHealthKeys;
    retainedHealthKeys.emplace(flexedge::node::OriginHealthRegistry::key("website", "origin"));
    healthState.publish(healthState.prepare(retainedHealthKeys));
    REQUIRE(healthState.healthy("website", "origin"));
    healthState.target("website", "origin").failure(1);
    const auto pendingRemoval = healthState.prepare({});
    REQUIRE(!healthState.healthy("website", "origin"));
    (void)pendingRemoval;
    const auto pendingRetention = healthState.prepare(retainedHealthKeys);
    healthState.target("website", "origin").success(1);
    healthState.publish(pendingRetention);
    REQUIRE(healthState.healthy("website", "origin"));
    healthState.target("website", "origin").failure(1);
    std::vector<std::thread> healthReporters;
    for (std::size_t index = 0; index < 8; ++index) {
        healthReporters.emplace_back([&] { healthState.target("website", "origin").success(1); });
    }
    for (auto& reporter : healthReporters) {
        reporter.join();
    }
    REQUIRE(healthState.healthy("website", "origin"));
    const auto retiredProbe = healthState.target("website", "origin");
    retainedHealthKeys.clear();
    healthState.publish(healthState.prepare(retainedHealthKeys));
    REQUIRE(healthState.healthy("website", "origin"));

    healthState.target("website", "origin").failure(1);
    healthState.target("website", "origin").success(1);
    healthState.target("website", "origin").recordProbe(false, 1, 42, "late result");
    REQUIRE(!healthState.claimDue("website", "origin", std::chrono::seconds(1)));
    REQUIRE(healthState.reports().empty());

    retainedHealthKeys.emplace(flexedge::node::OriginHealthRegistry::key("website", "origin"));
    healthState.publish(healthState.prepare(retainedHealthKeys));
    auto oldProbe = retiredProbe;
    oldProbe.failure(1);
    oldProbe.success(1);
    oldProbe.recordProbe(false, 1, 42, "retired probe");
    REQUIRE(healthState.healthy("website", "origin"));
    REQUIRE(healthState.reports().front().checkedAtUnixMillis == 0);
    std::atomic<bool> observeHealth{true};
    std::atomic<std::uint64_t> healthObservations{};
    std::latch readersStarted(8);
    std::vector<std::thread> healthReaders;
    for (std::size_t index = 0; index < 8; ++index) {
        healthReaders.emplace_back([&] {
            if (healthState.healthy("website", "origin")) {
                healthObservations.fetch_add(1, std::memory_order_relaxed);
            }
            readersStarted.count_down();
            while (observeHealth.load(std::memory_order_relaxed)) {
                if (healthState.healthy("website", "origin")) {
                    healthObservations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    readersStarted.wait();
    for (std::size_t index = 0; index < 64; ++index) {
        healthState.publish(healthState.prepare(retainedHealthKeys));
        healthState.target("website", "origin").success(1);
    }
    observeHealth.store(false, std::memory_order_relaxed);
    for (auto& reader : healthReaders) {
        reader.join();
    }
    REQUIRE(healthObservations.load(std::memory_order_relaxed) >= 8);
    REQUIRE(healthState.healthy("website", "origin"));

    flexedge::node::OriginHealthRegistry originHealth;
    originHealth.publish(originHealth.prepare(
        {flexedge::node::OriginHealthRegistry::key("website-1", "origin-1")}));
    originHealth.target("website-1", "origin-1").recordProbe(false, 1, 42, "timeout");
    const auto healthReports = originHealth.reports();
    REQUIRE(healthReports.size() == 1);
    REQUIRE(healthReports.front().status == "unhealthy");
    REQUIRE(healthReports.front().latencyMillis == 42);
    REQUIRE(healthReports.front().lastError == "timeout");
}
