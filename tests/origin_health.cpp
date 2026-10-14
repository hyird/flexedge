#include "node/data/origin_health.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <latch>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

static_assert(!std::is_default_constructible_v<flexedge::node::origin_health_registry::prepared_states>);

int main() {
    flexedge::node::origin_health_registry health_state;
    health_state.publish(health_state.prepare({flexedge::node::origin_health_registry::get_key("website", "origin")}));
    REQUIRE(health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").failure(2);
    REQUIRE(health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").failure(2);
    REQUIRE(!health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").success(2);
    REQUIRE(!health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").success(2);
    REQUIRE(health_state.healthy("website", "origin"));
    flexedge::node::origin_health_registry::key_set retained_health_keys;
    retained_health_keys.emplace(flexedge::node::origin_health_registry::get_key("website", "origin"));
    health_state.publish(health_state.prepare(retained_health_keys));
    REQUIRE(health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").failure(1);
    const auto pending_removal = health_state.prepare({});
    REQUIRE(!health_state.healthy("website", "origin"));
    (void)pending_removal;
    const auto pending_retention = health_state.prepare(retained_health_keys);
    health_state.get_target("website", "origin").success(1);
    health_state.publish(pending_retention);
    REQUIRE(health_state.healthy("website", "origin"));
    health_state.get_target("website", "origin").failure(1);
    std::vector<std::thread> health_reporters;
    for (std::size_t index = 0; index < 8; ++index) {
        health_reporters.emplace_back([&] { health_state.get_target("website", "origin").success(1); });
    }
    for (auto& reporter : health_reporters) {
        reporter.join();
    }
    REQUIRE(health_state.healthy("website", "origin"));
    const auto retired_probe = health_state.get_target("website", "origin");
    retained_health_keys.clear();
    health_state.publish(health_state.prepare(retained_health_keys));
    REQUIRE(health_state.healthy("website", "origin"));

    health_state.get_target("website", "origin").failure(1);
    health_state.get_target("website", "origin").success(1);
    health_state.get_target("website", "origin").record_probe(false, 1, 42, "late result");
    REQUIRE(!health_state.claim_due("website", "origin", std::chrono::seconds(1)));
    REQUIRE(health_state.reports().empty());

    retained_health_keys.emplace(flexedge::node::origin_health_registry::get_key("website", "origin"));
    health_state.publish(health_state.prepare(retained_health_keys));
    auto old_probe = retired_probe;
    old_probe.failure(1);
    old_probe.success(1);
    old_probe.record_probe(false, 1, 42, "retired probe");
    REQUIRE(health_state.healthy("website", "origin"));
    REQUIRE(health_state.reports().front().checked_at_unix_millis_ == 0);
    std::atomic<bool> observe_health{true};
    std::atomic<std::uint64_t> health_observations{};
    std::latch readers_started(8);
    std::vector<std::thread> health_readers;
    for (std::size_t index = 0; index < 8; ++index) {
        health_readers.emplace_back([&] {
            if (health_state.healthy("website", "origin")) {
                health_observations.fetch_add(1, std::memory_order_relaxed);
            }
            readers_started.count_down();
            while (observe_health.load(std::memory_order_relaxed)) {
                if (health_state.healthy("website", "origin")) {
                    health_observations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    readers_started.wait();
    for (std::size_t index = 0; index < 64; ++index) {
        health_state.publish(health_state.prepare(retained_health_keys));
        health_state.get_target("website", "origin").success(1);
    }
    observe_health.store(false, std::memory_order_relaxed);
    for (auto& reader : health_readers) {
        reader.join();
    }
    REQUIRE(health_observations.load(std::memory_order_relaxed) >= 8);
    REQUIRE(health_state.healthy("website", "origin"));

    flexedge::node::origin_health_registry origin_health;
    origin_health.publish(origin_health.prepare(
        {flexedge::node::origin_health_registry::get_key("website-1", "origin-1")}));
    origin_health.get_target("website-1", "origin-1").record_probe(false, 1, 42, "timeout");
    const auto health_reports = origin_health.reports();
    REQUIRE(health_reports.size() == 1);
    REQUIRE(health_reports.front().status_ == "unhealthy");
    REQUIRE(health_reports.front().latency_millis_ == 42);
    REQUIRE(health_reports.front().last_error_ == "timeout");
}
