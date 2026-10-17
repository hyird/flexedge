#include "node/data/origin_selection.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;

v2::Origin& add_origin(v2::Website& website,
    std::string_view id,
    std::string_view role = "primary",
    std::uint32_t weight = 1,
    std::string_view group = {},
    bool enabled = true) {
    auto& origin = *website.add_origins();
    origin.set_id(std::string(id));
    origin.set_role(std::string(role));
    origin.set_weight(weight);
    origin.set_group(std::string(group));
    origin.set_enabled(enabled);
    return origin;
}

std::shared_ptr<const origin_health_snapshot> health_for(const v2::Website& website,
    std::initializer_list<std::pair<std::string_view, bool>> entries) {
    origin_health_snapshot::origin_map origins;
    for (const auto& [id, healthy] : entries) {
        origins.emplace(origin_health_key{website.id(), std::string(id)},
            origin_health_snapshot::origin{.healthy_ = healthy});
    }
    return std::make_shared<const origin_health_snapshot>(std::move(origins));
}

void expect_order(const origin_plan& plan,
    std::uint64_t sequence,
    std::shared_ptr<const origin_health_snapshot> health,
    std::initializer_list<std::string_view> expected) {
    std::array<const v2::Origin*, 16> actual{};
    REQUIRE(expected.size() <= actual.size());
    origin_cursor cursor(plan, sequence, std::move(health));
    for (std::size_t index = 0; index < expected.size(); ++index) {
        actual[index] = cursor.next();
    }
    const auto* extra = cursor.next();
    const auto* after_exhaustion = cursor.next();
    REQUIRE(extra == nullptr);
    REQUIRE(after_exhaustion == nullptr);
    std::size_t index{};
    for (const auto id : expected) {
        REQUIRE(actual[index] != nullptr);
        REQUIRE(actual[index]->id() == id);
        ++index;
    }
}

void verify_weighted_cycles() {
    v2::Website website;
    website.set_id("weighted");
    add_origin(website, "p1", "primary", 2);
    add_origin(website, "b1", "backup", 2);
    add_origin(website, "p2", "primary", 3);
    add_origin(website, "b2", "backup", 1);
    add_origin(website, "p3", "primary", 1);
    const origin_plan plan(website, "default");
    REQUIRE(!plan.empty());
    expect_order(plan, 0, nullptr, {"p1", "p2", "p3", "b1", "b2"});
    expect_order(plan, 1, nullptr, {"p1", "p2", "p3", "b1", "b2"});
    expect_order(plan, 2, nullptr, {"p2", "p3", "p1", "b2", "b1"});
    expect_order(plan, 3, nullptr, {"p2", "p3", "p1", "b1", "b2"});
    expect_order(plan, 4, nullptr, {"p2", "p3", "p1", "b1", "b2"});
    expect_order(plan, 5, nullptr, {"p3", "p1", "p2", "b2", "b1"});
    expect_order(plan, 6, nullptr, {"p1", "p2", "p3", "b1", "b2"});
    expect_order(plan, std::numeric_limits<std::uint64_t>::max(), nullptr,
        {"p2", "p3", "p1", "b1", "b2"});

    // The returned objects are the immutable source objects, not request copies.
    origin_cursor cursor(plan, 2, nullptr);
    REQUIRE(cursor.next() == &website.origins(2));
}

void verify_health_partition() {
    v2::Website website;
    website.set_id("health");
    add_origin(website, "p1", "primary", 2);
    add_origin(website, "b1", "backup", 2);
    add_origin(website, "p2", "primary", 3);
    add_origin(website, "b2", "backup", 1);
    add_origin(website, "p3", "primary", 1);
    const origin_plan plan(website, "default");
    const auto mixed = health_for(website,
        {{"p1", true}, {"p2", false}, {"p3", false}, {"b1", false}, {"b2", true}});
    // Raw sequence 2 is p2,p3,p1,b2,b1. Health must partition that sequence
    // globally, not select a new weighted pivot among the healthy origins.
    expect_order(plan, 2, mixed, {"p1", "b2", "p2", "p3", "b1"});
    expect_order(plan, 5, mixed, {"p1", "b2", "p3", "p2", "b1"});
    expect_order(plan, 0, mixed, {"p1", "b2", "p2", "p3", "b1"});
    expect_order(plan, 2, health_for(website, {}), {"p2", "p3", "p1", "b2", "b1"});
    expect_order(plan, 2, health_for(website, {{"p2", false}}),
        {"p3", "p1", "b2", "b1", "p2"});
    expect_order(plan, 2,
        health_for(website, {{"p1", false}, {"p2", false}, {"p3", false}, {"b1", false}, {"b2", false}}),
        {"p2", "p3", "p1", "b2", "b1"});
    expect_order(plan, 2,
        health_for(website, {{"p1", true}, {"p2", true}, {"p3", true}, {"b1", true}, {"b2", true}}),
        {"p2", "p3", "p1", "b2", "b1"});

    origin_health_snapshot::origin_map foreign;
    foreign.emplace(origin_health_key{"other-website", "p2"},
        origin_health_snapshot::origin{.healthy_ = false});
    expect_order(plan, 2, std::make_shared<const origin_health_snapshot>(std::move(foreign)),
        {"p2", "p3", "p1", "b2", "b1"});

    auto health = health_for(website, {{"p2", false}});
    const std::weak_ptr<const origin_health_snapshot> retained = health;
    origin_cursor cursor(plan, 2, health);
    health.reset();
    REQUIRE(!retained.expired());
    REQUIRE(cursor.next()->id() == "p3");
    // A later publication cannot change this request's captured snapshot.
    health = health_for(website, {{"p3", false}});
    REQUIRE(cursor.next()->id() == "p1");
    REQUIRE(cursor.next()->id() == "b2");
    REQUIRE(cursor.next()->id() == "b1");
    REQUIRE(cursor.next()->id() == "p2");
    REQUIRE(cursor.next() == nullptr);
    cursor = origin_cursor{};
    REQUIRE(retained.expired());
}

void verify_selection_and_filtering() {
    v2::Website website;
    website.set_id("groups");
    website.set_default_origin_group("custom");
    add_origin(website, "first", "primary", 1, "a");
    add_origin(website, "disabled", "primary", 1, "a", false);
    add_origin(website, "zero", "primary", 0, "a");
    add_origin(website, "unknown-role", "standby", 1, "a");
    add_origin(website, "second", "primary", 1, "b");
    add_origin(website, "third", "backup", 1, "b");
    add_origin(website, "implicit-default");
    add_origin(website, "explicit-default", "primary", 1, "default");
    add_origin(website, "custom-default", "primary", 1, "custom");
    const origin_plan grouped(website, "a");
    expect_order(grouped, 0, nullptr, {"first"});
    expect_order(origin_plan(website, "b"), 0, nullptr, {"second", "third"});
    expect_order(origin_plan(website, ""), 0, nullptr, {"implicit-default", "explicit-default"});
    expect_order(origin_plan(website, "default"), 0, nullptr, {"implicit-default", "explicit-default"});
    expect_order(origin_plan(website, website.default_origin_group()), 0, nullptr, {"custom-default"});

    // IDs override the group but not source order; duplicates do not repeat an origin.
    const std::array<std::string, 9> ids{
        "third", "second", "first", "second", "disabled", "zero", "unknown-role", "missing", "third"};
    const origin_plan selected(website, "nonexistent", ids);
    expect_order(selected, 0, nullptr, {"first", "second", "third"});
    expect_order(selected, 1, nullptr, {"second", "first", "third"});
    const std::array<std::string, 4> excluded{"disabled", "zero", "unknown-role", "missing"};
    const origin_plan empty_ids(website, "a", excluded);
    REQUIRE(empty_ids.empty());
    expect_order(empty_ids, 0, nullptr, {});
    const origin_plan empty_group(website, "nonexistent");
    REQUIRE(empty_group.empty());
    expect_order(empty_group, 1, nullptr, {});
}

void verify_empty_roles_and_plans() {
    origin_cursor cursor;
    REQUIRE(cursor.next() == nullptr);
    REQUIRE(cursor.next() == nullptr);
    const origin_plan empty;
    REQUIRE(empty.empty());
    expect_order(empty, 0, nullptr, {});
    v2::Website website;
    website.set_id("empty");
    {
        const origin_plan no_origins(website, "default");
        REQUIRE(no_origins.empty());
        expect_order(no_origins, std::numeric_limits<std::uint64_t>::max(), nullptr, {});
    }

    add_origin(website, "backup-first", "backup", 1);
    add_origin(website, "backup-second", "backup", 2);
    const origin_plan backup_only(website, "default");
    expect_order(backup_only, 1, nullptr, {"backup-second", "backup-first"});
    expect_order(backup_only, 1, health_for(website, {{"backup-second", false}}),
        {"backup-first", "backup-second"});
    expect_order(backup_only, 1,
        health_for(website, {{"backup-first", false}, {"backup-second", false}}),
        {"backup-second", "backup-first"});

    v2::Website primary;
    primary.set_id("primary");
    add_origin(primary, "only");
    const origin_plan primary_only(primary, "default");
    expect_order(primary_only, std::numeric_limits<std::uint64_t>::max(), nullptr, {"only"});
    expect_order(primary_only, 0, health_for(primary, {{"only", false}}), {"only"});
}

void verify_wide_weights() {
    v2::Website website;
    website.set_id("wide");
    constexpr auto weight = std::numeric_limits<std::uint32_t>::max();
    constexpr auto wide_weight = static_cast<std::uint64_t>(weight);
    add_origin(website, "p1", "primary", weight);
    add_origin(website, "p2", "primary", weight);
    add_origin(website, "b1", "backup", weight);
    add_origin(website, "b2", "backup", 1);
    const origin_plan plan(website, "default");
    expect_order(plan, wide_weight - 1, nullptr, {"p1", "p2", "b1", "b2"});
    expect_order(plan, wide_weight, nullptr, {"p2", "p1", "b2", "b1"});
    expect_order(plan, wide_weight + 1, nullptr, {"p2", "p1", "b1", "b2"});
    expect_order(plan, 2 * wide_weight - 1, nullptr, {"p2", "p1", "b1", "b2"});
    expect_order(plan, 2 * wide_weight, nullptr, {"p1", "p2", "b1", "b2"});
}
}  // namespace

int main() {
    try {
        verify_weighted_cycles();
        verify_health_partition();
        verify_selection_and_filtering();
        verify_empty_roles_and_plans();
        verify_wide_weights();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
