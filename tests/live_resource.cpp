#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/live_resource/snapshot_cache.h"

#define REQUIRE(condition)                                                                  \
    do {                                                                                    \
        if (!(condition))                                                                   \
            throw std::runtime_error("live resource fanout invariant failed: " #condition); \
    } while (false)

namespace {

using hub = service::live_resource::hub;
using resource = service::live_resource::resource;
using cache = service::live_resource::snapshot_cache;
using snapshot = service::live_resource::snapshot;

std::shared_ptr<const snapshot> get_snapshot(std::string bytes, bool failure = false) {
    return std::make_shared<const snapshot>(snapshot{std::move(bytes), failure});
}

template <typename loop_type, typename read_type>
cache::result receive_result(const loop_type& loop, read_type& read) {
    auto outcome = loop.start(read.receiver_.receive_for(std::chrono::seconds(1))).get();
    REQUIRE(outcome.has_value());
    return outcome.value();
}

ruvia::task<ruvia::worker_wait_result<std::uint64_t>> receive(
    hub::subscription& subscription, std::chrono::milliseconds timeout) {
    co_return co_await subscription.receive_for(timeout, {});
}

void run_query_key_tests() {
    using service::live_resource::query_key;
    REQUIRE(query_key("list", 2, 25) == "s4:listi2;i25;");
    REQUIRE(query_key("list", std::optional<std::string>{}, std::optional<std::string>{"a:b"}) ==
            "s4:list-+s3:a:b");
    REQUIRE(query_key("list", 1) != query_key("list", "1"));
}

void run_snapshot_cache_tests() {
    using service::live_resource::snapshot_capacity_error;
    ruvia::event_loop_pool loops({.loop_count_ = 2});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    loops.start();

    cache cache_value;
    const auto key = service::live_resource::snapshot_key{"tenant-a", resource::nodes, "node-a",
        service::live_resource::query_key("detail")};
    auto leader = cache_value.subscribe(key);
    auto follower = cache_value.subscribe(key);
    auto first_read = leader.read(first.handle());
    auto second_read = follower.read(second.handle());
    REQUIRE(first_read.ticket_.has_value());
    REQUIRE(!second_read.ticket_.has_value());
    first_read.ticket_->complete(get_snapshot("same-bytes"));
    const auto first_result = receive_result(first, first_read);
    const auto second_result = receive_result(second, second_read);
    REQUIRE(first_result.snapshot_ && second_result.snapshot_);
    REQUIRE(first_result.snapshot_ == second_result.snapshot_);
    REQUIRE(first_result.snapshot_->data_ == "same-bytes");
    REQUIRE(leader.current(first_result));
    auto hit = cache_value.subscribe(key).read(first.handle());
    REQUIRE(!hit.ticket_.has_value() && hit.ready_.snapshot_->data_ == "same-bytes");

    // Query, tenant, resource and row id all participate in cache identity.
    auto list = cache_value.subscribe({"tenant-a", resource::nodes, {},
        service::live_resource::query_key("list", 1, 20)});
    auto other_query = cache_value.subscribe({"tenant-a", resource::nodes, {},
        service::live_resource::query_key("list", 2, 20)});
    auto other_tenant = cache_value.subscribe({"tenant-b", resource::nodes, "node-a",
        service::live_resource::query_key("detail")});
    auto other_resource = cache_value.subscribe({"tenant-a", resource::clusters, "node-a",
        service::live_resource::query_key("detail")});
    auto other_id = cache_value.subscribe({"tenant-a", resource::nodes, "node-b",
        service::live_resource::query_key("detail")});
    REQUIRE(cache_value.get_stats().groups_ == 6);

    cache_value.invalidate("tenant-a", resource::nodes, "node-a");
    auto pending = leader.read(first.handle());
    REQUIRE(pending.ticket_.has_value());
    cache_value.invalidate("tenant-a", resource::nodes, "node-a");
    pending.ticket_->complete(get_snapshot("stale"));
    const auto stale = receive_result(first, pending);
    REQUIRE(!stale.snapshot_ && stale.version_ == 0);
    auto fresh = leader.read(first.handle());
    REQUIRE(fresh.ticket_.has_value());
    fresh.ticket_->complete(get_snapshot("fresh"));
    REQUIRE(receive_result(first, fresh).snapshot_->data_ == "fresh");
    auto list_read = list.read(first.handle());
    auto tenant_read = other_tenant.read(first.handle());
    auto resource_read = other_resource.read(first.handle());
    auto id_read = other_id.read(first.handle());
    REQUIRE(list_read.ticket_.has_value() && tenant_read.ticket_.has_value() &&
            resource_read.ticket_.has_value() && id_read.ticket_.has_value());
    list_read.ticket_->complete(get_snapshot("list"));
    tenant_read.ticket_->complete(get_snapshot("tenant"));
    resource_read.ticket_->complete(get_snapshot("resource"));
    id_read.ticket_->complete(get_snapshot("id"));
    (void)receive_result(first, list_read);
    (void)receive_result(first, tenant_read);
    (void)receive_result(first, resource_read);
    (void)receive_result(first, id_read);
    cache_value.invalidate("tenant-a", resource::nodes, "node-a", true);
    auto list_hit = list.read(first.handle());
    auto tenant_hit = other_tenant.read(first.handle());
    auto resource_hit = other_resource.read(first.handle());
    auto id_hit = other_id.read(first.handle());
    REQUIRE(!list_hit.ticket_.has_value() && list_hit.ready_.snapshot_);
    REQUIRE(!tenant_hit.ticket_.has_value() && tenant_hit.ready_.snapshot_);
    REQUIRE(!resource_hit.ticket_.has_value() && resource_hit.ready_.snapshot_);
    REQUIRE(!id_hit.ticket_.has_value() && id_hit.ready_.snapshot_);
    auto detail_after = leader.read(first.handle());
    REQUIRE(detail_after.ticket_.has_value());
    detail_after.ticket_->complete(get_snapshot("detail-after"));
    (void)receive_result(first, detail_after);

    // A canceled waiter is weakly held and does not block another consumer.
    cache canceled_cache;
    auto canceled_leader = canceled_cache.subscribe(key);
    auto canceled_follower = canceled_cache.subscribe(key);
    auto leader_read = canceled_leader.read(first.handle());
    auto follower_read = canceled_follower.read(second.handle());
    auto leader_ticket = std::move(leader_read.ticket_);
    leader_read.waiter_.reset();
    leader_read.receiver_.close();
    REQUIRE(leader_ticket.has_value() && !follower_read.ticket_.has_value());
    leader_ticket->complete(get_snapshot("remaining"));
    REQUIRE(receive_result(second, follower_read).snapshot_->data_ == "remaining");

    // TTL expiry and failed reads are lazy and never leave a permanent hit.
    cache::limits ttl_limits;
    ttl_limits.max_age_ = std::chrono::seconds(5);
    cache ttl(ttl_limits);
    auto ttl_lease = ttl.subscribe(key);
    auto ttl_read = ttl_lease.read(first.handle());
    ttl_read.ticket_->complete(get_snapshot("ttl"));
    const auto ttl_result = receive_result(first, ttl_read);
    REQUIRE(!ttl_lease.read(first.handle(), cache::clock::now() + std::chrono::seconds(1)).ticket_);
    auto expired = ttl_lease.read(first.handle(), cache::clock::now() + std::chrono::seconds(6));
    REQUIRE(expired.ticket_.has_value());
    expired.ticket_->complete(get_snapshot("expired"));
    REQUIRE(receive_result(first, expired).snapshot_->data_ == "expired");
    auto failed = ttl_lease.read(first.handle(), cache::clock::now() + std::chrono::seconds(7));
    REQUIRE(failed.ticket_.has_value());
    failed.ticket_->complete(get_snapshot("db-error", true));
    REQUIRE(receive_result(first, failed).snapshot_->failure_);
    auto retry = ttl_lease.read(first.handle(), cache::clock::now() + std::chrono::seconds(8));
    REQUIRE(retry.ticket_.has_value());
    retry.ticket_->complete(get_snapshot("recovered"));
    REQUIRE(receive_result(first, retry).snapshot_->data_ == "recovered");

    // Byte LRU, per-snapshot size, group, subscriber, flight and key caps.
    cache::limits limits;
    limits.bytes_ = 4;
    cache lru(limits);
    auto lru_a = lru.subscribe({"t", resource::nodes, "a", "s1:a"});
    auto lru_b = lru.subscribe({"t", resource::nodes, "b", "s1:b"});
    auto lru_c = lru.subscribe({"t", resource::nodes, "c", "s1:c"});
    for (auto* lease : {&lru_a, &lru_b}) {
        auto read = lease->read(first.handle());
        read.ticket_->complete(get_snapshot("aa"));
        (void)receive_result(first, read);
    }
    auto lru_read = lru_c.read(first.handle());
    lru_read.ticket_->complete(get_snapshot("cc"));
    (void)receive_result(first, lru_read);
    REQUIRE(lru.get_stats().bytes_ == 4);
    auto lru_a_read = lru_a.read(first.handle());
    REQUIRE(lru_a_read.ticket_.has_value());
    lru_a_read.ticket_->complete(get_snapshot("aa"));
    (void)receive_result(first, lru_a_read);

    cache::limits size_limits;
    size_limits.snapshot_bytes_ = 2;
    cache sized(size_limits);
    auto sized_lease = sized.subscribe(key);
    auto oversized = sized_lease.read(first.handle());
    oversized.ticket_->complete(get_snapshot("123"));
    (void)receive_result(first, oversized);
    REQUIRE(sized.get_stats().bytes_ == 0);
    REQUIRE(sized_lease.read(first.handle()).ticket_.has_value());

    cache::limits cap_limits;
    cap_limits.groups_ = 1;
    cap_limits.subscribers_ = 1;
    cap_limits.flights_ = 1;
    cap_limits.key_bytes_ = 4;
    cache caps(cap_limits);
    auto cap_lease = caps.subscribe({"t", resource::nodes, {}, "q"});
    bool rejected = false;
    try {
        (void)caps.subscribe({"t2", resource::nodes, {}, "q"});
    } catch (const snapshot_capacity_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
    rejected = false;
    try {
        (void)caps.subscribe({"tenant", resource::nodes, {}, "q"});
    } catch (const snapshot_capacity_error&) {
        rejected = true;
    }
    REQUIRE(rejected);
    auto cap_read = cap_lease.read(first.handle());
    REQUIRE(cap_read.ticket_.has_value());
    auto cap_read2 = cap_lease.read(second.handle());
    REQUIRE(!cap_read2.ticket_.has_value());

    cache::limits group_limits;
    group_limits.groups_ = 1;
    cache group_caps(group_limits);
    auto group_lease = group_caps.subscribe({"t", resource::nodes, {}, "q"});
    rejected = false;
    try {
        (void)group_caps.subscribe({"t", resource::nodes, "x", "q"});
    } catch (const snapshot_capacity_error&) {
        rejected = true;
    }
    REQUIRE(rejected);

    cache::limits subscriber_limits;
    subscriber_limits.subscribers_ = 1;
    cache subscriber_caps(subscriber_limits);
    auto subscriber_lease = subscriber_caps.subscribe({"t", resource::nodes, {}, "q"});
    rejected = false;
    try {
        (void)subscriber_caps.subscribe({"t", resource::nodes, {}, "q"});
    } catch (const snapshot_capacity_error&) {
        rejected = true;
    }
    REQUIRE(rejected);

    // Dropping the last lease erases the entry and rejects an in-flight result.
    cache disconnect;
    std::optional<cache::lease> old_lease;
    old_lease.emplace(disconnect.subscribe(key));
    auto old_read = old_lease->read(first.handle());
    auto old_ticket = std::move(old_read.ticket_);
    old_read.waiter_.reset();
    old_read.receiver_.close();
    old_lease.reset();
    auto new_lease = disconnect.subscribe(key);
    auto new_read = new_lease.read(second.handle());
    REQUIRE(new_read.ticket_.has_value());
    old_ticket->complete(get_snapshot("old"));
    new_read.ticket_->complete(get_snapshot("new"));
    REQUIRE(receive_result(second, new_read).snapshot_->data_ == "new");

    // A returning reader freezes the patch boundary it has already replayed.
    // A flight begun before that boundary is rejected; the next flight is
    // accepted even if another patch arrives while it is running.
    cache boundary_cache;
    auto boundary_lease = boundary_cache.subscribe(key);
    auto boundary_read = boundary_lease.read(first.handle());
    const auto boundary_ticket = std::move(boundary_read.ticket_);
    boundary_cache.invalidate("tenant-a", resource::nodes, "node-a", false, true);
    const auto boundary = boundary_lease.patch_epoch();
    boundary_ticket->complete(get_snapshot("before-boundary"));
    const auto before_boundary = receive_result(first, boundary_read);
    REQUIRE(!boundary_lease.current(before_boundary, boundary));
    auto after_boundary_read = boundary_lease.read(first.handle());
    REQUIRE(after_boundary_read.ticket_.has_value());
    boundary_cache.invalidate("tenant-a", resource::nodes, "node-a", false, true);
    after_boundary_read.ticket_->complete(get_snapshot("after-boundary"));
    const auto after_boundary = receive_result(first, after_boundary_read);
    REQUIRE(boundary_lease.current(after_boundary, boundary));

    // hub notifications invalidate the cache that belongs to each
    // subscription, including detail-only runtime and origin updates.
    hub hub;
    auto node_sub = hub.subscribe(first.handle(), "hub-tenant", resource::nodes, "node-1",
        service::live_resource::query_key("detail"));
    auto node_read = node_sub.snapshots().read(first.handle());
    node_read.ticket_->complete(get_snapshot("node"));
    (void)receive_result(first, node_read);
    hub.publish_runtime("hub-tenant", "node-1", "runtime");
    auto node_after_runtime = node_sub.snapshots().read(first.handle());
    REQUIRE(node_after_runtime.ticket_.has_value());
    node_after_runtime.ticket_->complete(get_snapshot("node-runtime"));
    (void)receive_result(first, node_after_runtime);

    // A high-rate runtime patch stream keeps the baseline readable by the
    // subscriber that started the query, while a late join retries once.
    hub.publish_runtime("hub-tenant", "node-1", "runtime-reset");
    auto stress_read = node_sub.snapshots().read(first.handle());
    REQUIRE(stress_read.ticket_.has_value());
    for (int i = 0; i < 100; ++i) {
        hub.publish_runtime("hub-tenant", "node-1", "runtime-" + std::to_string(i));
    }
    auto late_node = hub.subscribe(second.handle(), "hub-tenant", resource::nodes, "node-1",
        service::live_resource::query_key("detail"));
    auto late_read = late_node.snapshots().read(second.handle());
    REQUIRE(!late_read.ticket_.has_value());
    stress_read.ticket_->complete(get_snapshot("baseline"));
    const auto baseline = receive_result(first, stress_read);
    const auto late_baseline = receive_result(second, late_read);
    REQUIRE(node_sub.snapshots().current(baseline));
    REQUIRE(!late_node.snapshots().current(late_baseline));
    REQUIRE(baseline.snapshot_ && late_baseline.snapshot_);
    const auto runtime_signal = first.start(receive(node_sub, std::chrono::seconds(1))).get();
    REQUIRE(runtime_signal.has_value());
    REQUIRE(node_sub.drain().runtime_.at("node-1") == "runtime-99");

    auto late_retry = late_node.snapshots().read(second.handle());
    REQUIRE(late_retry.ticket_.has_value());
    for (int i = 100; i < 200; ++i) {
        hub.publish_runtime("hub-tenant", "node-1", "runtime-" + std::to_string(i));
    }
    late_retry.ticket_->complete(get_snapshot("late-baseline"));
    const auto late_accepted = receive_result(second, late_retry);
    REQUIRE(late_node.snapshots().current(late_accepted));
    const auto late_signal = second.start(receive(late_node, std::chrono::seconds(1))).get();
    REQUIRE(late_signal.has_value());
    REQUIRE(late_node.drain().runtime_.at("node-1") == "runtime-199");

    auto website_detail = hub.subscribe(first.handle(), "hub-tenant", resource::websites, "site-1",
        service::live_resource::query_key("detail"));
    auto website_list = hub.subscribe(first.handle(), "hub-tenant", resource::websites, {},
        service::live_resource::query_key("list"));
    auto website_detail_read = website_detail.snapshots().read(first.handle());
    website_detail_read.ticket_->complete(get_snapshot("site-detail"));
    (void)receive_result(first, website_detail_read);
    auto website_list_read = website_list.snapshots().read(first.handle());
    website_list_read.ticket_->complete(get_snapshot("site-list"));
    (void)receive_result(first, website_list_read);
    hub.publish_origins("hub-tenant", "site-1", "node-1", "origin");
    auto website_detail_after = website_detail.snapshots().read(first.handle());
    auto website_list_after = website_list.snapshots().read(first.handle());
    REQUIRE(website_detail_after.ticket_.has_value());
    REQUIRE(!website_list_after.ticket_.has_value() && website_list_after.ready_.snapshot_);
    website_detail_after.ticket_->complete(get_snapshot("site-origin"));
    (void)receive_result(first, website_detail_after);

    loops.stop();
    loops.join();
}

template <typename loop_type>
bool signalled(const loop_type& loop, hub::subscription& subscription,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
    const auto result = loop.start(receive(subscription, timeout)).get();
    REQUIRE(result.has_value() || result.status() == ruvia::worker_wait_status::timed_out);
    return result.has_value();
}

// One connection carries several projections; each wakes only for its own
// source, and the traffic projection coalesces high-rate changes.
void run_combined_and_coalesced_tests() {
    using clock = std::chrono::steady_clock;
    using service::live_resource::query_key;
    using service::live_resource::subscription_part;
    ruvia::event_loop_pool loops({.loop_count_ = 2});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    loops.start();
    const auto quiet = std::chrono::milliseconds(20);
    const auto interval = std::chrono::seconds(30);
    const auto start = clock::now();

    hub hub;
    // No subscriber: nothing is scheduled, so nothing is ever read.
    hub.publish_coalesced("tenant", resource::overview_traffic, interval, start);

    std::vector<subscription_part> parts;
    parts.push_back({resource::overview, {}, query_key("overview")});
    parts.push_back({resource::overview_traffic, {}, query_key("traffic")});
    parts.push_back({resource::nodes, {}, query_key("list", 1, 100)});
    auto combined = hub.subscribe(first.handle(), "tenant", std::move(parts));
    REQUIRE(combined.parts() == 3);
    auto traffic = hub.subscribe(second.handle(), "tenant", resource::overview_traffic, {},
        query_key("traffic"));
    auto other_tenant = hub.subscribe(second.handle(), "other", resource::overview_traffic, {},
        query_key("traffic"));
    // The gate is per tenant: the earlier unobserved publish left no delivery
    // window behind, so the first observed change is delivered at once.
    hub.publish_coalesced("tenant", resource::overview_traffic, interval, start);
    REQUIRE(signalled(first, combined));
    REQUIRE(!combined.drain(0).snapshot_);
    const auto immediate = combined.drain(1);
    REQUIRE(immediate.snapshot_ && !immediate.refresh_at_);
    REQUIRE(!combined.drain(2).snapshot_);
    REQUIRE(signalled(second, traffic));
    REQUIRE(traffic.drain().snapshot_);
    REQUIRE(!signalled(second, other_tenant, quiet));

    // A burst of access-log notifications inside the interval schedules one
    // deferred delivery and reads nothing now.
    for (int index = 0; index < 100; ++index) {
        hub.publish_coalesced("tenant", resource::overview_traffic, interval,
            start + std::chrono::milliseconds(100 + index));
    }
    REQUIRE(signalled(first, combined));
    const auto deferred = combined.drain(1);
    REQUIRE(!deferred.snapshot_ && deferred.refresh_at_ == start + interval);
    REQUIRE(!combined.drain(0).snapshot_ && !combined.drain(2).snapshot_);
    REQUIRE(!signalled(first, combined, quiet));
    REQUIRE(signalled(second, traffic));
    REQUIRE(traffic.drain().refresh_at_ == start + interval);

    // Website changes wake the counts at once but share the traffic gate.
    hub.publish("tenant", resource::websites, "website");
    REQUIRE(signalled(first, combined));
    REQUIRE(combined.drain(0).snapshot_);
    REQUIRE(!combined.drain(1).snapshot_ && !combined.drain(2).snapshot_);
    REQUIRE(!signalled(second, traffic, quiet));

    // Runtime patches reach only the node part; node changes wake counts and
    // the node page, never traffic.
    hub.publish_runtime("tenant", "node", "runtime");
    REQUIRE(signalled(first, combined));
    REQUIRE(!combined.drain(0).snapshot_ && combined.drain(0).runtime_.empty());
    REQUIRE(combined.drain(1).runtime_.empty());
    REQUIRE(combined.drain(2).runtime_.at("node") == "runtime");
    hub.publish("tenant", resource::nodes, "node");
    REQUIRE(signalled(first, combined));
    REQUIRE(combined.drain(0).snapshot_ && !combined.drain(1).snapshot_ &&
            combined.drain(2).snapshot_);

    // Release is due only at the deadline; the first release delivers once to
    // every subscriber and later releases of the same deferral do nothing.
    REQUIRE(!combined.release_coalesced(1, start + interval - std::chrono::milliseconds(1)));
    REQUIRE(combined.release_coalesced(1, start + interval));
    REQUIRE(!traffic.release_coalesced(0, start + interval));
    REQUIRE(signalled(first, combined));
    REQUIRE(combined.drain(1).snapshot_ && !combined.drain(0).snapshot_);
    REQUIRE(signalled(second, traffic));
    REQUIRE(traffic.drain().snapshot_);
    REQUIRE(!signalled(second, other_tenant, quiet));

    // A subscriber joining during a deferral learns its deadline.
    hub.publish_coalesced("tenant", resource::overview_traffic, interval,
        start + interval + std::chrono::seconds(1));
    REQUIRE(signalled(first, combined));
    REQUIRE(combined.drain(1).refresh_at_ == start + interval * 2);
    auto late = hub.subscribe(first.handle(), "tenant", resource::overview_traffic, {},
        query_key("traffic"));
    REQUIRE(signalled(first, late));
    REQUIRE(late.drain().refresh_at_ == start + interval * 2);

    loops.stop();
    loops.join();
}

}  // namespace

int main() {
    try {
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto first_loop = loops.loop(0);
        const auto second_loop = loops.loop(1);
        hub fanout;
        std::optional<hub::subscription> node;
        node.emplace(fanout.subscribe(first_loop.handle(), "tenant-a", resource::nodes, "node-a"));
        auto node_list = fanout.subscribe(second_loop.handle(), "tenant-a", resource::nodes);
        auto other_tenant = fanout.subscribe(second_loop.handle(), "tenant-b", resource::nodes, "node-a");
        loops.start();

        fanout.publish("tenant-a", resource::access_history, "website-a");
        REQUIRE(first_loop.start(receive(*node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        fanout.publish("tenant-b", resource::nodes, "node-a");
        REQUIRE(first_loop.start(receive(*node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(second_loop.start(receive(other_tenant, std::chrono::seconds(1))).get().has_value());

        fanout.publish("tenant-a", resource::nodes, "node-a");
        REQUIRE(first_loop.start(receive(*node, std::chrono::seconds(1))).get().has_value());
        REQUIRE(second_loop.start(receive(node_list, std::chrono::seconds(1))).get().has_value());

        fanout.publish_runtime("tenant-a", "node-a", "runtime-1");
        fanout.publish_runtime("tenant-a", "node-a", "runtime-2");
        fanout.publish_runtime("tenant-a", "node-b", "runtime-b");
        fanout.publish("tenant-a", resource::nodes, "node-a");
        const auto signal = first_loop.start(receive(*node, std::chrono::seconds(1))).get();
        REQUIRE(signal.has_value());
        const auto pending = node->drain();
        REQUIRE(pending.snapshot_);
        REQUIRE(pending.runtime_.size() == 1);
        REQUIRE(pending.runtime_.at("node-a") == "runtime-2");
        const auto list_pending = node_list.drain();
        REQUIRE(list_pending.snapshot_);
        REQUIRE(list_pending.runtime_.size() == 2);
        REQUIRE(list_pending.runtime_.at("node-b") == "runtime-b");
        const auto drained = node->drain();
        REQUIRE(!drained.snapshot_ && drained.runtime_.empty());

        auto overview = fanout.subscribe(first_loop.handle(), "tenant-a", resource::overview);
        auto website = fanout.subscribe(first_loop.handle(), "tenant-a", resource::websites, "website-a");
        auto website_list = fanout.subscribe(second_loop.handle(), "tenant-a", resource::websites);
        auto other_website = fanout.subscribe(second_loop.handle(), "tenant-a", resource::websites, "website-b");
        auto zones = fanout.subscribe(first_loop.handle(), "tenant-a", resource::dns_zones);
        fanout.publish_runtime("tenant-a", "node-a", "runtime-3");
        REQUIRE(first_loop.start(receive(overview, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(first_loop.start(receive(website, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        fanout.publish_origins("tenant-a", "website-a", "node-a", "origin-state");
        REQUIRE(first_loop.start(receive(website, std::chrono::seconds(1))).get().has_value());
        const auto origin = website.drain();
        REQUIRE(!origin.snapshot_ && origin.origins_.at("node-a") == "origin-state");
        REQUIRE(second_loop.start(receive(website_list, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(second_loop.start(receive(other_website, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        // Provider verification changes zone availability without changing a
        // zone row; the joined projection must refresh on that committed event.
        fanout.publish("tenant-a", resource::providers, "provider-a");
        REQUIRE(first_loop.start(receive(zones, std::chrono::seconds(1))).get().has_value());
        REQUIRE(zones.drain().snapshot_);
        REQUIRE(first_loop.start(receive(overview, std::chrono::seconds(1))).get().has_value());
        REQUIRE(overview.drain().snapshot_);

        node.reset();
        fanout.publish("tenant-a", resource::nodes, "node-a");

        // Subtask attempt notices reach only incremental parts of exactly that
        // operation in that tenant; other operation subscriptions just re-read.
        using service::live_resource::subscription_part;
        using service::live_resource::task_attempt_notice;
        std::vector<subscription_part> history_parts;
        history_parts.push_back({resource::operations, "task-a", service::live_resource::query_key("history", "s"), true});
        history_parts.push_back({resource::operations, "task-a", service::live_resource::query_key("detail")});
        auto history = fanout.subscribe(first_loop.handle(), "tenant-a", std::move(history_parts));
        auto other_task = fanout.subscribe(first_loop.handle(), "tenant-a", resource::operations, "task-b");
        auto task_list = fanout.subscribe(second_loop.handle(), "tenant-a", resource::operations);
        std::vector<subscription_part> foreign_parts;
        foreign_parts.push_back({resource::operations, "task-a", service::live_resource::query_key("history", "s"), true});
        auto foreign_history = fanout.subscribe(second_loop.handle(), "tenant-b", std::move(foreign_parts));
        fanout.publish_task_attempt("tenant-a", "task-a", {.subtask_id_ = "s", .event_id_ = 7});
        REQUIRE(first_loop.start(receive(history, std::chrono::seconds(1))).get().has_value());
        const auto attempt = history.drain(0);
        REQUIRE(attempt.task_attempts_.size() == 1 && attempt.task_attempts_[0].event_id_ == 7 &&
                !attempt.history_reset_ && !attempt.recovery_);
        REQUIRE(history.drain(1).snapshot_ && history.drain(1).task_attempts_.empty());
        REQUIRE(second_loop.start(receive(task_list, std::chrono::seconds(1))).get().has_value());
        REQUIRE(task_list.drain().task_attempts_.empty());
        REQUIRE(first_loop.start(receive(other_task, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        REQUIRE(second_loop.start(receive(foreign_history, std::chrono::milliseconds(20))).get().status() ==
                ruvia::worker_wait_status::timed_out);
        // A part that falls behind drops its notices for one recovery snapshot.
        for (std::int64_t event = 0; event <= static_cast<std::int64_t>(service::live_resource::max_pending_task_attempts);
            ++event) {
            fanout.publish_task_attempt("tenant-a", "task-a", {.subtask_id_ = "s", .event_id_ = event});
        }
        const auto overflow = history.drain(0);
        REQUIRE(overflow.history_reset_ && overflow.task_attempts_.empty());
        fanout.reset_task_history("tenant-a", "task-a");
        REQUIRE(history.drain(0).history_reset_);
        // A recovery reaches every subscription of the tenant once, marked so
        // the stream writes it even when its bytes are unchanged.
        fanout.recover("tenant-a");
        const auto recovered = history.drain(0);
        REQUIRE(recovered.snapshot_ && recovered.history_reset_ && recovered.recovery_);
        const auto other_recovered = other_task.drain();
        REQUIRE(other_recovered.snapshot_ && other_recovered.recovery_ && !foreign_history.drain().snapshot_);
        // Ordinary changes are not recoveries.
        fanout.publish("tenant-a", resource::operations, "task-b");
        const auto ordinary = other_task.drain();
        REQUIRE(ordinary.snapshot_ && !ordinary.recovery_);

        // A stream part writes a snapshot only when its bytes changed, however
        // often the cache re-read it; forgetting (initial, recovery) writes again.
        service::live_resource::written_snapshot written;
        REQUIRE(written.replace(R"({"name":"a"})"));
        REQUIRE(!written.replace(R"({"name":"a"})"));
        REQUIRE(!written.replace(std::string(R"({"name":"a"})")));
        REQUIRE(written.replace(R"({"name":"b"})"));
        REQUIRE(!written.replace(R"({"name":"b"})"));
        REQUIRE(written.replace(R"({"name":"a"})"));
        written.forget();
        REQUIRE(written.replace(R"({"name":"a"})"));
        REQUIRE(!written.replace(R"({"name":"a"})"));
        REQUIRE(written.replace(std::string(R"({"name":"a"})") + std::string(1, '\0')));

        // Stream entries round-trip; oversized lists degrade to a recovery.
        namespace notifications = service::live_resource::notifications;
        notifications::resource_notification sent("tenant-a");
        sent.changed(resource::websites, "site")
            .changed(resource::operations)
            .task_attempt("task-a", {.subtask_id_ = "s", .event_id_ = 9})
            .task_history_reset("task-b")
            .node_runtime("node-a", R"({"id":"node-a"})")
            .origin_runtime("site", R"({"id":"site"})")
            .node_config();
        const auto fields = notifications::encode(notifications::bounded(sent));
        const auto parsed = notifications::decode(fields);
        REQUIRE(parsed && parsed->tenant_id_ == "tenant-a" && parsed->changes_ == sent.changes_ &&
                parsed->task_attempts_.size() == 1 && parsed->task_attempts_[0].notice_.event_id_ == 9 &&
                parsed->task_attempts_[0].notice_.subtask_id_ == "s" && parsed->task_history_resets_[0] == "task-b" &&
                parsed->node_id_ == "node-a" && parsed->node_runtime_ == R"({"id":"node-a"})" &&
                parsed->origin_runtime_.size() == 1 && parsed->node_config_ && !parsed->missed_until_);
        notifications::resource_notification many("tenant-a");
        for (std::size_t index = 0; index <= notifications::max_items; ++index) {
            many.changed(resource::websites, "site-" + std::to_string(index));
        }
        const auto degraded = notifications::bounded(many);
        REQUIRE(degraded.changes_.empty() && degraded.missed_until_);

        run_query_key_tests();
        run_snapshot_cache_tests();
        run_combined_and_coalesced_tests();

        loops.stop();
        loops.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
