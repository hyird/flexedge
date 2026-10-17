#include "node/data/origin_health.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

static_assert(!std::is_default_constructible_v<flexedge::node::origin_health_state::prepared>);
static_assert(!std::is_copy_constructible_v<flexedge::node::origin_health_state::prepared>);

namespace {

using flexedge::node::origin_health_key;
using flexedge::node::origin_health_key_set;
using flexedge::node::origin_health_key_view;
using flexedge::node::origin_health_state;

const origin_health_key_view origin_key{"website", "origin"};

origin_health_key_set keys_of(origin_health_key_view key) {
    return {origin_health_key{.website_id_ = std::string(key.website_id_),
        .origin_id_ = std::string(key.origin_id_)}};
}

std::uint64_t id_of(const origin_health_state& state, origin_health_key_view key) {
    const auto snapshot = state.snapshot();
    const auto* origin = snapshot->find(key);
    REQUIRE(origin != nullptr);
    return origin->id_;
}

bool healthy(const origin_health_state& state, origin_health_key_view key) {
    return state.snapshot()->healthy(key);
}

}  // namespace

int main() {
    origin_health_state health_state;
    {
        auto next = health_state.prepare(keys_of(origin_key), {});
        REQUIRE(health_state.snapshot()->find(origin_key) == nullptr);
        health_state.activate(std::move(next));
    }
    const auto id = id_of(health_state, origin_key);
    REQUIRE(healthy(health_state, origin_key));
    REQUIRE(!health_state.record(origin_key, id, true, 2));

    // Consecutive outcomes cross the threshold; partial streaks do not change the snapshot.
    REQUIRE(!health_state.record(origin_key, id, false, 2));
    REQUIRE(healthy(health_state, origin_key));
    REQUIRE(health_state.record(origin_key, id, false, 2));
    REQUIRE(!healthy(health_state, origin_key));
    REQUIRE(!health_state.snapshot()->all_healthy());
    REQUIRE(!health_state.record(origin_key, id, true, 2));
    REQUIRE(!healthy(health_state, origin_key));
    REQUIRE(health_state.record(origin_key, id, true, 2));
    REQUIRE(healthy(health_state, origin_key));
    REQUIRE(health_state.snapshot()->all_healthy());

    // Retained memberships keep their state and id, including results recorded while the
    // next configuration is pending.
    REQUIRE(health_state.record(origin_key, id, false, 1));
    {
        auto pending_removal = health_state.prepare({}, {});
        REQUIRE(!healthy(health_state, origin_key));
        (void)pending_removal;
    }
    {
        auto pending_retention = health_state.prepare(keys_of(origin_key), {});
        REQUIRE(health_state.record(origin_key, id, true, 1));
        health_state.activate(std::move(pending_retention));
    }
    REQUIRE(id_of(health_state, origin_key) == id);
    REQUIRE(healthy(health_state, origin_key));

    // A retired origin ignores late outcomes and probe results and is not probed.
    const auto retired_probe = health_state.probe_target(origin_key);
    health_state.activate(health_state.prepare({}, {}));
    REQUIRE(health_state.snapshot()->find(origin_key) == nullptr);
    REQUIRE(healthy(health_state, origin_key));
    REQUIRE(!health_state.record(origin_key, id, false, 1));
    auto late_probe = retired_probe;
    late_probe.record_probe(false, 1, 42, "late result");
    REQUIRE(!health_state.claim_due(origin_key, std::chrono::seconds(1)));
    REQUIRE(health_state.snapshot()->reports().empty());

    // Re-adding it is a new membership: outcomes of the old one stay discarded.
    health_state.activate(health_state.prepare(keys_of(origin_key), {}));
    const auto readded_id = id_of(health_state, origin_key);
    REQUIRE(readded_id != id);
    REQUIRE(!health_state.record(origin_key, id, false, 1));
    late_probe.record_probe(false, 1, 42, "retired probe");
    REQUIRE(healthy(health_state, origin_key));
    REQUIRE(health_state.snapshot()->reports().front().checked_at_unix_millis_ == 0);
    REQUIRE(health_state.snapshot()->reports().front().status_ == "unknown");

    REQUIRE(health_state.claim_due(origin_key, std::chrono::seconds(60)));
    REQUIRE(!health_state.claim_due(origin_key, std::chrono::seconds(60)));
    REQUIRE(health_state.claim_due(origin_key, std::chrono::seconds(0)));

    // Snapshots are immutable: later changes do not alter one already taken.
    const auto before_probe = health_state.snapshot();
    health_state.probe_target(origin_key).record_probe(false, 1, 42, "timeout");
    REQUIRE(before_probe->healthy(origin_key));
    const auto health_reports = health_state.snapshot()->reports();
    REQUIRE(health_reports.size() == 1);
    REQUIRE(health_reports.front().website_id_ == "website");
    REQUIRE(health_reports.front().origin_id_ == "origin");
    REQUIRE(health_reports.front().status_ == "unhealthy");
    REQUIRE(health_reports.front().latency_millis_ == 42);
    REQUIRE(health_reports.front().last_error_ == "timeout");
    REQUIRE(health_reports.front().checked_at_unix_millis_ > 0);
}
