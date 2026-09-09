#include <chrono>
#include <memory>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <ruvia/core/EventLoopPool.h>

#include "service/features/live_resource/fanout.h"
#include "service/features/live_resource/snapshot_cache.h"

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("live resource fanout invariant failed: " #condition);       \
    } while (false)

namespace {

using Hub = service::live_resource::Hub;
using Resource = service::live_resource::Resource;
using Cache = service::live_resource::SnapshotCache;
using Snapshot = service::live_resource::Snapshot;

std::shared_ptr<const Snapshot> snapshot(std::string bytes, bool failure = false) {
    return std::make_shared<const Snapshot>(Snapshot{std::move(bytes), failure});
}

template <typename Loop, typename Read>
Cache::Result receiveResult(const Loop& loop, Read& read) {
    auto outcome = loop.start(read.receiver.receiveFor(std::chrono::seconds(1))).get();
    REQUIRE(outcome.hasValue());
    return outcome.value();
}

ruvia::Task<ruvia::WorkerWaitResult<std::uint64_t>> receive(
    Hub::Subscription& subscription, std::chrono::milliseconds timeout) {
    co_return co_await subscription.receiveFor(timeout, {});
}

void runQueryKeyTests() {
    using service::live_resource::queryKey;
    REQUIRE(queryKey("list", 2, 25) == "s4:listi2;i25;");
    REQUIRE(queryKey("list", std::optional<std::string>{}, std::optional<std::string>{"a:b"}) ==
            "s4:list-+s3:a:b");
    REQUIRE(queryKey("list", 1) != queryKey("list", "1"));
}

void runSnapshotCacheTests() {
    using service::live_resource::SnapshotCapacityError;
    ruvia::EventLoopPool loops({.loopCount = 2});
    const auto first = loops.loop(0);
    const auto second = loops.loop(1);
    loops.start();

    Cache cache;
    const auto key = service::live_resource::SnapshotKey{"tenant-a", Resource::nodes, "node-a",
                                                         service::live_resource::queryKey("detail")};
    auto leader = cache.subscribe(key);
    auto follower = cache.subscribe(key);
    auto firstRead = leader.read(first.handle());
    auto secondRead = follower.read(second.handle());
    REQUIRE(firstRead.ticket.has_value());
    REQUIRE(!secondRead.ticket.has_value());
    firstRead.ticket->complete(snapshot("same-bytes"));
    const auto firstResult = receiveResult(first, firstRead);
    const auto secondResult = receiveResult(second, secondRead);
    REQUIRE(firstResult.snapshot && secondResult.snapshot);
    REQUIRE(firstResult.snapshot == secondResult.snapshot);
    REQUIRE(firstResult.snapshot->data == "same-bytes");
    REQUIRE(leader.current(firstResult));
    auto hit = cache.subscribe(key).read(first.handle());
    REQUIRE(!hit.ticket.has_value() && hit.ready.snapshot->data == "same-bytes");

    // Query, tenant, resource and row id all participate in cache identity.
    auto list = cache.subscribe({"tenant-a", Resource::nodes, {},
                                 service::live_resource::queryKey("list", 1, 20)});
    auto otherQuery = cache.subscribe({"tenant-a", Resource::nodes, {},
                                       service::live_resource::queryKey("list", 2, 20)});
    auto otherTenant = cache.subscribe({"tenant-b", Resource::nodes, "node-a",
                                        service::live_resource::queryKey("detail")});
    auto otherResource = cache.subscribe({"tenant-a", Resource::clusters, "node-a",
                                          service::live_resource::queryKey("detail")});
    auto otherId = cache.subscribe({"tenant-a", Resource::nodes, "node-b",
                                    service::live_resource::queryKey("detail")});
    REQUIRE(cache.stats().groups == 6);

    cache.invalidate("tenant-a", Resource::nodes, "node-a");
    auto pending = leader.read(first.handle());
    REQUIRE(pending.ticket.has_value());
    cache.invalidate("tenant-a", Resource::nodes, "node-a");
    pending.ticket->complete(snapshot("stale"));
    const auto stale = receiveResult(first, pending);
    REQUIRE(!stale.snapshot && stale.version == 0);
    auto fresh = leader.read(first.handle());
    REQUIRE(fresh.ticket.has_value());
    fresh.ticket->complete(snapshot("fresh"));
    REQUIRE(receiveResult(first, fresh).snapshot->data == "fresh");
    auto listRead = list.read(first.handle());
    auto tenantRead = otherTenant.read(first.handle());
    auto resourceRead = otherResource.read(first.handle());
    auto idRead = otherId.read(first.handle());
    REQUIRE(listRead.ticket.has_value() && tenantRead.ticket.has_value() &&
            resourceRead.ticket.has_value() && idRead.ticket.has_value());
    listRead.ticket->complete(snapshot("list"));
    tenantRead.ticket->complete(snapshot("tenant"));
    resourceRead.ticket->complete(snapshot("resource"));
    idRead.ticket->complete(snapshot("id"));
    (void)receiveResult(first, listRead);
    (void)receiveResult(first, tenantRead);
    (void)receiveResult(first, resourceRead);
    (void)receiveResult(first, idRead);
    cache.invalidate("tenant-a", Resource::nodes, "node-a", true);
    auto listHit = list.read(first.handle());
    auto tenantHit = otherTenant.read(first.handle());
    auto resourceHit = otherResource.read(first.handle());
    auto idHit = otherId.read(first.handle());
    REQUIRE(!listHit.ticket.has_value() && listHit.ready.snapshot);
    REQUIRE(!tenantHit.ticket.has_value() && tenantHit.ready.snapshot);
    REQUIRE(!resourceHit.ticket.has_value() && resourceHit.ready.snapshot);
    REQUIRE(!idHit.ticket.has_value() && idHit.ready.snapshot);
    auto detailAfter = leader.read(first.handle());
    REQUIRE(detailAfter.ticket.has_value());
    detailAfter.ticket->complete(snapshot("detail-after"));
    (void)receiveResult(first, detailAfter);

    // A canceled waiter is weakly held and does not block another consumer.
    Cache canceledCache;
    auto canceledLeader = canceledCache.subscribe(key);
    auto canceledFollower = canceledCache.subscribe(key);
    auto leaderRead = canceledLeader.read(first.handle());
    auto followerRead = canceledFollower.read(second.handle());
    auto leaderTicket = std::move(leaderRead.ticket);
    leaderRead.waiter.reset();
    leaderRead.receiver.close();
    REQUIRE(leaderTicket.has_value() && !followerRead.ticket.has_value());
    leaderTicket->complete(snapshot("remaining"));
    REQUIRE(receiveResult(second, followerRead).snapshot->data == "remaining");

    // TTL expiry and failed reads are lazy and never leave a permanent hit.
    Cache::Limits ttlLimits;
    ttlLimits.maxAge = std::chrono::seconds(5);
    Cache ttl(ttlLimits);
    auto ttlLease = ttl.subscribe(key);
    auto ttlRead = ttlLease.read(first.handle());
    ttlRead.ticket->complete(snapshot("ttl"));
    const auto ttlResult = receiveResult(first, ttlRead);
    REQUIRE(!ttlLease.read(first.handle(), Cache::Clock::now() + std::chrono::seconds(1)).ticket);
    auto expired = ttlLease.read(first.handle(), Cache::Clock::now() + std::chrono::seconds(6));
    REQUIRE(expired.ticket.has_value());
    expired.ticket->complete(snapshot("expired"));
    REQUIRE(receiveResult(first, expired).snapshot->data == "expired");
    auto failed = ttlLease.read(first.handle(), Cache::Clock::now() + std::chrono::seconds(7));
    REQUIRE(failed.ticket.has_value());
    failed.ticket->complete(snapshot("db-error", true));
    REQUIRE(receiveResult(first, failed).snapshot->failure);
    auto retry = ttlLease.read(first.handle(), Cache::Clock::now() + std::chrono::seconds(8));
    REQUIRE(retry.ticket.has_value());
    retry.ticket->complete(snapshot("recovered"));
    REQUIRE(receiveResult(first, retry).snapshot->data == "recovered");

    // Byte LRU, per-snapshot size, group, subscriber, flight and key caps.
    Cache::Limits limits;
    limits.bytes = 4;
    Cache lru(limits);
    auto lruA = lru.subscribe({"t", Resource::nodes, "a", "s1:a"});
    auto lruB = lru.subscribe({"t", Resource::nodes, "b", "s1:b"});
    auto lruC = lru.subscribe({"t", Resource::nodes, "c", "s1:c"});
    for (auto* lease : {&lruA, &lruB}) {
        auto read = lease->read(first.handle());
        read.ticket->complete(snapshot("aa"));
        (void)receiveResult(first, read);
    }
    auto lruRead = lruC.read(first.handle());
    lruRead.ticket->complete(snapshot("cc"));
    (void)receiveResult(first, lruRead);
    REQUIRE(lru.stats().bytes == 4);
    auto lruARead = lruA.read(first.handle());
    REQUIRE(lruARead.ticket.has_value());
    lruARead.ticket->complete(snapshot("aa"));
    (void)receiveResult(first, lruARead);

    Cache::Limits sizeLimits;
    sizeLimits.snapshotBytes = 2;
    Cache sized(sizeLimits);
    auto sizedLease = sized.subscribe(key);
    auto oversized = sizedLease.read(first.handle());
    oversized.ticket->complete(snapshot("123"));
    (void)receiveResult(first, oversized);
    REQUIRE(sized.stats().bytes == 0);
    REQUIRE(sizedLease.read(first.handle()).ticket.has_value());

    Cache::Limits capLimits;
    capLimits.groups = 1;
    capLimits.subscribers = 1;
    capLimits.flights = 1;
    capLimits.keyBytes = 4;
    Cache caps(capLimits);
    auto capLease = caps.subscribe({"t", Resource::nodes, {}, "q"});
    bool rejected = false;
    try { (void)caps.subscribe({"t2", Resource::nodes, {}, "q"}); } catch (const SnapshotCapacityError&) { rejected = true; }
    REQUIRE(rejected);
    rejected = false;
    try { (void)caps.subscribe({"tenant", Resource::nodes, {}, "q"}); } catch (const SnapshotCapacityError&) { rejected = true; }
    REQUIRE(rejected);
    auto capRead = capLease.read(first.handle());
    REQUIRE(capRead.ticket.has_value());
    auto capRead2 = capLease.read(second.handle());
    REQUIRE(!capRead2.ticket.has_value());

    Cache::Limits groupLimits;
    groupLimits.groups = 1;
    Cache groupCaps(groupLimits);
    auto groupLease = groupCaps.subscribe({"t", Resource::nodes, {}, "q"});
    rejected = false;
    try { (void)groupCaps.subscribe({"t", Resource::nodes, "x", "q"}); } catch (const SnapshotCapacityError&) { rejected = true; }
    REQUIRE(rejected);

    Cache::Limits subscriberLimits;
    subscriberLimits.subscribers = 1;
    Cache subscriberCaps(subscriberLimits);
    auto subscriberLease = subscriberCaps.subscribe({"t", Resource::nodes, {}, "q"});
    rejected = false;
    try { (void)subscriberCaps.subscribe({"t", Resource::nodes, {}, "q"}); } catch (const SnapshotCapacityError&) { rejected = true; }
    REQUIRE(rejected);

    // Dropping the last lease erases the entry and rejects an in-flight result.
    Cache disconnect;
    std::optional<Cache::Lease> oldLease;
    oldLease.emplace(disconnect.subscribe(key));
    auto oldRead = oldLease->read(first.handle());
    auto oldTicket = std::move(oldRead.ticket);
    oldRead.waiter.reset();
    oldRead.receiver.close();
    oldLease.reset();
    auto newLease = disconnect.subscribe(key);
    auto newRead = newLease.read(second.handle());
    REQUIRE(newRead.ticket.has_value());
    oldTicket->complete(snapshot("old"));
    newRead.ticket->complete(snapshot("new"));
    REQUIRE(receiveResult(second, newRead).snapshot->data == "new");

    // A returning reader freezes the patch boundary it has already replayed.
    // A flight begun before that boundary is rejected; the next flight is
    // accepted even if another patch arrives while it is running.
    Cache boundaryCache;
    auto boundaryLease = boundaryCache.subscribe(key);
    auto boundaryRead = boundaryLease.read(first.handle());
    const auto boundaryTicket = std::move(boundaryRead.ticket);
    boundaryCache.invalidate("tenant-a", Resource::nodes, "node-a", false, true);
    const auto boundary = boundaryLease.patchEpoch();
    boundaryTicket->complete(snapshot("before-boundary"));
    const auto beforeBoundary = receiveResult(first, boundaryRead);
    REQUIRE(!boundaryLease.current(beforeBoundary, boundary));
    auto afterBoundaryRead = boundaryLease.read(first.handle());
    REQUIRE(afterBoundaryRead.ticket.has_value());
    boundaryCache.invalidate("tenant-a", Resource::nodes, "node-a", false, true);
    afterBoundaryRead.ticket->complete(snapshot("after-boundary"));
    const auto afterBoundary = receiveResult(first, afterBoundaryRead);
    REQUIRE(boundaryLease.current(afterBoundary, boundary));

    // Hub notifications invalidate the cache that belongs to each
    // subscription, including detail-only runtime and origin updates.
    Hub hub;
    auto nodeSub = hub.subscribe(first.handle(), "hub-tenant", Resource::nodes, "node-1",
                                 service::live_resource::queryKey("detail"));
    auto nodeRead = nodeSub.snapshots().read(first.handle());
    nodeRead.ticket->complete(snapshot("node"));
    (void)receiveResult(first, nodeRead);
    hub.publishRuntime("hub-tenant", "node-1", "runtime");
    auto nodeAfterRuntime = nodeSub.snapshots().read(first.handle());
    REQUIRE(nodeAfterRuntime.ticket.has_value());
    nodeAfterRuntime.ticket->complete(snapshot("node-runtime"));
    (void)receiveResult(first, nodeAfterRuntime);

    // A high-rate runtime patch stream keeps the baseline readable by the
    // subscriber that started the query, while a late join retries once.
    hub.publishRuntime("hub-tenant", "node-1", "runtime-reset");
    auto stressRead = nodeSub.snapshots().read(first.handle());
    REQUIRE(stressRead.ticket.has_value());
    for (int i = 0; i < 100; ++i)
        hub.publishRuntime("hub-tenant", "node-1", "runtime-" + std::to_string(i));
    auto lateNode = hub.subscribe(second.handle(), "hub-tenant", Resource::nodes, "node-1",
                                  service::live_resource::queryKey("detail"));
    auto lateRead = lateNode.snapshots().read(second.handle());
    REQUIRE(!lateRead.ticket.has_value());
    stressRead.ticket->complete(snapshot("baseline"));
    const auto baseline = receiveResult(first, stressRead);
    const auto lateBaseline = receiveResult(second, lateRead);
    REQUIRE(nodeSub.snapshots().current(baseline));
    REQUIRE(!lateNode.snapshots().current(lateBaseline));
    REQUIRE(baseline.snapshot && lateBaseline.snapshot);
    const auto runtimeSignal = first.start(receive(nodeSub, std::chrono::seconds(1))).get();
    REQUIRE(runtimeSignal.hasValue());
    REQUIRE(nodeSub.drain().runtime.at("node-1") == "runtime-99");

    auto lateRetry = lateNode.snapshots().read(second.handle());
    REQUIRE(lateRetry.ticket.has_value());
    for (int i = 100; i < 200; ++i)
        hub.publishRuntime("hub-tenant", "node-1", "runtime-" + std::to_string(i));
    lateRetry.ticket->complete(snapshot("late-baseline"));
    const auto lateAccepted = receiveResult(second, lateRetry);
    REQUIRE(lateNode.snapshots().current(lateAccepted));
    const auto lateSignal = second.start(receive(lateNode, std::chrono::seconds(1))).get();
    REQUIRE(lateSignal.hasValue());
    REQUIRE(lateNode.drain().runtime.at("node-1") == "runtime-199");

    auto websiteDetail = hub.subscribe(first.handle(), "hub-tenant", Resource::websites, "site-1",
                                       service::live_resource::queryKey("detail"));
    auto websiteList = hub.subscribe(first.handle(), "hub-tenant", Resource::websites, {},
                                     service::live_resource::queryKey("list"));
    auto websiteDetailRead = websiteDetail.snapshots().read(first.handle());
    websiteDetailRead.ticket->complete(snapshot("site-detail"));
    (void)receiveResult(first, websiteDetailRead);
    auto websiteListRead = websiteList.snapshots().read(first.handle());
    websiteListRead.ticket->complete(snapshot("site-list"));
    (void)receiveResult(first, websiteListRead);
    hub.publishOrigins("hub-tenant", "site-1", "node-1", "origin");
    auto websiteDetailAfter = websiteDetail.snapshots().read(first.handle());
    auto websiteListAfter = websiteList.snapshots().read(first.handle());
    REQUIRE(websiteDetailAfter.ticket.has_value());
    REQUIRE(!websiteListAfter.ticket.has_value() && websiteListAfter.ready.snapshot);
    websiteDetailAfter.ticket->complete(snapshot("site-origin"));
    (void)receiveResult(first, websiteDetailAfter);

    loops.stop();
    loops.join();
}

} // namespace

int main() {
    try {
        ruvia::EventLoopPool loops({.loopCount = 2});
        const auto firstLoop = loops.loop(0);
        const auto secondLoop = loops.loop(1);
        Hub fanout;
        std::optional<Hub::Subscription> node;
        node.emplace(fanout.subscribe(firstLoop.handle(), "tenant-a", Resource::nodes, "node-a"));
        auto nodeList = fanout.subscribe(secondLoop.handle(), "tenant-a", Resource::nodes);
        auto otherTenant = fanout.subscribe(secondLoop.handle(), "tenant-b", Resource::nodes, "node-a");
        loops.start();

        fanout.publish("tenant-a", Resource::accessHistory, "website-a");
        REQUIRE(firstLoop.start(receive(*node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        fanout.publish("tenant-b", Resource::nodes, "node-a");
        REQUIRE(firstLoop.start(receive(*node, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(secondLoop.start(receive(otherTenant, std::chrono::seconds(1))).get().hasValue());

        fanout.publish("tenant-a", Resource::nodes, "node-a");
        REQUIRE(firstLoop.start(receive(*node, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(secondLoop.start(receive(nodeList, std::chrono::seconds(1))).get().hasValue());

        fanout.publishRuntime("tenant-a", "node-a", "runtime-1");
        fanout.publishRuntime("tenant-a", "node-a", "runtime-2");
        fanout.publishRuntime("tenant-a", "node-b", "runtime-b");
        fanout.publish("tenant-a", Resource::nodes, "node-a");
        const auto signal = firstLoop.start(receive(*node, std::chrono::seconds(1))).get();
        REQUIRE(signal.hasValue());
        const auto pending = node->drain();
        REQUIRE(pending.snapshot);
        REQUIRE(pending.runtime.size() == 1);
        REQUIRE(pending.runtime.at("node-a") == "runtime-2");
        const auto listPending = nodeList.drain();
        REQUIRE(listPending.snapshot);
        REQUIRE(listPending.runtime.size() == 2);
        REQUIRE(listPending.runtime.at("node-b") == "runtime-b");
        const auto drained = node->drain();
        REQUIRE(!drained.snapshot && drained.runtime.empty());

        auto overview = fanout.subscribe(firstLoop.handle(), "tenant-a", Resource::overview);
        auto website = fanout.subscribe(firstLoop.handle(), "tenant-a", Resource::websites, "website-a");
        auto websiteList = fanout.subscribe(secondLoop.handle(), "tenant-a", Resource::websites);
        auto otherWebsite = fanout.subscribe(secondLoop.handle(), "tenant-a", Resource::websites, "website-b");
        auto zones = fanout.subscribe(firstLoop.handle(), "tenant-a", Resource::dnsZones);
        fanout.publishRuntime("tenant-a", "node-a", "runtime-3");
        REQUIRE(firstLoop.start(receive(overview, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(firstLoop.start(receive(website, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        fanout.publishOrigins("tenant-a", "website-a", "node-a", "origin-state");
        REQUIRE(firstLoop.start(receive(website, std::chrono::seconds(1))).get().hasValue());
        const auto origin = website.drain();
        REQUIRE(!origin.snapshot && origin.origins.at("node-a") == "origin-state");
        REQUIRE(secondLoop.start(receive(websiteList, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        REQUIRE(secondLoop.start(receive(otherWebsite, std::chrono::milliseconds(20))).get().status() ==
                ruvia::WorkerWaitStatus::kTimedOut);
        // Provider verification changes zone availability without changing a
        // zone row; the joined projection must refresh on that committed event.
        fanout.publish("tenant-a", Resource::providers, "provider-a");
        REQUIRE(firstLoop.start(receive(zones, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(zones.drain().snapshot);
        REQUIRE(firstLoop.start(receive(overview, std::chrono::seconds(1))).get().hasValue());
        REQUIRE(overview.drain().snapshot);

        node.reset();
        fanout.publish("tenant-a", Resource::nodes, "node-a");

        runQueryKeyTests();
        runSnapshotCacheTests();

        loops.stop();
        loops.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
