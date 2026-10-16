#include "service/features/live_resource/redis_snapshot_cache.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis_client.h>

#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "support/redis_test.h"

namespace {

namespace live = service::live_resource;
namespace cache = service::live_resource::redis_snapshot_cache;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("redis snapshot cache: " + std::string(message));
    }
}

live::snapshot_key overview_key(std::string tenant) {
    return {std::move(tenant), live::resource::overview, {}, live::query_key("overview")};
}

live::snapshot_key dashboard_key(std::string tenant, std::string id) {
    return {std::move(tenant), live::resource::access_history, std::move(id), live::query_key("dashboard")};
}

ruvia::task<void> clear(ruvia::redis_client& redis) {
    for (const auto key : cache::keys) {
        (void)co_await redis.handle().del(key);
    }
}

ruvia::task<cache::lookup> read(ruvia::redis_client& redis, const live::snapshot_key& key) {
    co_return co_await cache::read(redis.handle(), key);
}

ruvia::task<bool> store(ruvia::redis_client& redis, const live::snapshot_key& key,
    std::string generation, live::snapshot value) {
    co_return co_await cache::store(redis.handle(), key, generation, value);
}

ruvia::task<void> invalidate(ruvia::redis_client& redis, std::vector<std::string> scopes) {
    co_await cache::invalidate(redis.handle(), scopes);
}

ruvia::task<void> expire_entry(ruvia::redis_client& redis, const live::snapshot_key& key) {
    (void)co_await redis.handle().zadd(cache::keys[1], 0, cache::entry_key(key));
}

ruvia::task<void> corrupt_values(ruvia::redis_client& redis) {
    (void)co_await redis.handle().del(cache::keys[0]);
    (void)co_await redis.handle().set(cache::keys[0], "wrong-type");
}

ruvia::task<void> fill_and_verify_capacity(ruvia::redis_client& redis) {
    const auto handle = redis.handle();
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < cache::max_entries + 1; ++index) {
        const auto key = dashboard_key("capacity", std::to_string(index));
        auto candidate = co_await cache::read(handle, key);
        require(co_await cache::store(handle, key, candidate.generation_, {"{}", false}), "capacity insert remains current");
    }
    const auto entries = co_await handle.hlen(cache::keys[0]);
    const auto indexed = co_await handle.zcard(cache::keys[1]);
    require(entries <= static_cast<std::int64_t>(cache::max_entries) && entries == indexed,
        "entry capacity and expiry index remain bounded");
    require(!(co_await cache::read(handle, dashboard_key("capacity", "0"))).snapshot_, "oldest entry is evicted");
    for (std::size_t index = 0; index != 80; ++index) {
        const auto key = dashboard_key("bytes", std::to_string(index));
        auto candidate = co_await cache::read(handle, key);
        require(co_await cache::store(handle, key, candidate.generation_, {std::string(cache::max_snapshot_bytes, 'x'), false}),
            "byte-capacity insert remains current");
    }
    const auto accounted = co_await handle.get(cache::keys[2]);
    require(accounted && std::stoull(std::string(*accounted)) <= cache::max_bytes, "serialized bytes remain bounded");
    const auto stored = co_await handle.hvals(cache::keys[0]);
    std::size_t actual{};
    for (const auto& value : stored) {
        actual += value.size();
    }
    require(accounted && actual == std::stoull(std::string(*accounted)), "byte budget matches retained values after eviction");
    require(co_await handle.hlen(cache::keys[3]) <= static_cast<std::int64_t>(cache::max_scopes),
        "generation metadata is bounded");
    std::cout << "capacity: " << entries << " entries, retained bytes=" << actual << ", elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() << '\n';
}

// This counter represents calls to the expensive source loader, not SQL
// timing. Cross-worker/cache hits must completely bypass that loader.
ruvia::task<void> source_loads(ruvia::redis_client& redis, const live::snapshot_key& key,
    std::size_t& loads, std::size_t requests) {
    const auto handle = redis.handle();
    for (std::size_t index = 0; index < requests; ++index) {
        auto candidate = co_await cache::read(handle, key);
        if (!candidate.snapshot_) {
            ++loads;
            require(co_await cache::store(handle, key, candidate.generation_, {"source-result", false}), "source flight stays current");
        }
    }
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto first = loops.loop(0);
        const auto second = loops.loop(1);
        ruvia::redis_client redis_a(first, *config);
        ruvia::redis_client redis_b(second, *config);
        loops.start();
        first.start(redis_a.connect()).get();
        second.start(redis_b.connect()).get();
        first.start(clear(redis_a)).get();

        const auto tenant_a = overview_key("tenant-a");
        const auto tenant_b = overview_key("tenant-b");
        const auto website_a = dashboard_key("tenant-a", "website-a");
        const auto website_b = dashboard_key("tenant-a", "website-b");
        require(cache::cacheable(tenant_a) && cache::cacheable(website_a), "expensive projections are allowlisted");
        auto filtered = tenant_a;
        filtered.query_ = live::query_key("overview", "private-filter");
        require(!cache::cacheable(filtered), "unknown filters and permission shapes bypass the shared cache");
        auto collision_a = tenant_a;
        collision_a.tenant_ = "a:b";
        collision_a.id_ = "c";
        auto collision_b = tenant_a;
        collision_b.tenant_ = "a";
        collision_b.id_ = "b:c";
        require(cache::entry_key(collision_a) != cache::entry_key(collision_b), "length framing prevents key collisions");

        const auto cold = first.start(read(redis_a, tenant_a)).get();
        require(!cold.snapshot_, "first worker misses");
        require(first.start(store(redis_a, tenant_a, cold.generation_, {"counts-a", false})).get(), "first worker fills");
        const auto cross_worker = second.start(read(redis_b, tenant_a)).get();
        require(cross_worker.snapshot_ && cross_worker.snapshot_->data_ == "counts-a", "another worker and Redis client hits");
        require(cross_worker.max_age_ > std::chrono::seconds::zero() && cross_worker.max_age_ <= cache::max_age,
            "read never extends the retained Redis lifetime");
        require(!second.start(read(redis_b, tenant_b)).get().snapshot_, "tenant cache keys are isolated");
        require(!second.start(read(redis_b, filtered)).get().snapshot_, "query keys are isolated");

        const auto website_flight = first.start(read(redis_a, website_a)).get();
        const auto unaffected = first.start(read(redis_a, website_b)).get();
        require(first.start(store(redis_a, website_b, unaffected.generation_, {"website-b", false})).get(), "other website is cached");
        second.start(invalidate(redis_b, cache::invalidation_scopes("tenant-a", live::resource::access_history, "website-a"))).get();
        require(!first.start(store(redis_a, website_a, website_flight.generation_, {"old-flight", false})).get(),
            "a commit in another worker fences an old database flight");
        require(!first.start(read(redis_a, website_a)).get().snapshot_, "invalidated bytes cannot reappear");
        require(second.start(read(redis_b, website_b)).get().snapshot_.has_value(), "single website invalidation keeps unrelated website");
        require(second.start(read(redis_b, tenant_a)).get().snapshot_.has_value(), "log invalidation keeps resource counts");

        second.start(invalidate(redis_b, {cache::tenant_scope("tenant-a")})).get();
        require(!first.start(read(redis_a, tenant_a)).get().snapshot_, "tenant recovery invalidates counts");
        require(!first.start(read(redis_a, website_b)).get().snapshot_, "tenant recovery invalidates dashboard");
        require(!first.start(store(redis_a, tenant_a, cold.generation_, {"late-counts", false})).get(), "tenant recovery fences existing flight");

        const auto global_flight = first.start(read(redis_a, tenant_b)).get();
        second.start(invalidate(redis_b, {"*"})).get();
        require(!first.start(store(redis_a, tenant_b, global_flight.generation_, {"late-global", false})).get(),
            "stream recovery fences every tenant");

        auto rejected = first.start(read(redis_a, website_a)).get();
        require(first.start(store(redis_a, website_a, rejected.generation_, {"failure", true})).get(), "source failures do not fail cache bookkeeping");
        require(!second.start(read(redis_b, website_a)).get().snapshot_, "source failures are never cached");
        require(first.start(store(redis_a, website_a, rejected.generation_, {std::string(cache::max_snapshot_bytes + 1, 'x'), false})).get(),
            "oversized response remains deliverable");
        require(!second.start(read(redis_b, website_a)).get().snapshot_, "oversized response is not retained");
        require(first.start(store(redis_a, website_a, rejected.generation_, {"expiring", false})).get(), "expiring response is stored");
        second.start(expire_entry(redis_b, website_a)).get();
        require(!first.start(read(redis_a, website_a)).get().snapshot_, "logical TTL removes expired bytes");

        const auto evicted_flight = first.start(read(redis_a, tenant_a)).get();
        std::vector<std::string> unrelated;
        unrelated.reserve(cache::max_scopes + 1);
        for (std::size_t index = 0; index <= cache::max_scopes; ++index) {
            unrelated.push_back(cache::tenant_scope("unrelated-" + std::to_string(index)));
        }
        second.start(invalidate(redis_b, std::move(unrelated))).get();
        const auto recreated = first.start(read(redis_a, tenant_a)).get();
        require(recreated.generation_ != evicted_flight.generation_, "evicted generation is recreated with a unique token");
        require(!first.start(store(redis_a, tenant_a, evicted_flight.generation_, {"aba-flight", false})).get(),
            "metadata eviction cannot allow an ABA refill");

        first.start(clear(redis_a)).get();
        const auto before_flush = first.start(read(redis_a, tenant_a)).get();
        first.start(clear(redis_a)).get();
        const auto after_flush = second.start(read(redis_b, tenant_a)).get();
        require(before_flush.generation_ != after_flush.generation_, "cache loss never reuses a generation token");
        require(!first.start(store(redis_a, tenant_a, before_flush.generation_, {"before-flush", false})).get(),
            "cache loss fences a flight from before Redis recovery");
        std::size_t loads{};
        const auto sample_start = std::chrono::steady_clock::now();
        first.start(source_loads(redis_a, tenant_a, loads, 1)).get();
        second.start(source_loads(redis_b, tenant_a, loads, 1000)).get();
        require(loads == 1, "1001 reads on two workers invoke the source loader once");
        std::cout << "source-loader sample: requests=1001, source_loads=" << loads << ", elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sample_start).count() << '\n';

        first.start(fill_and_verify_capacity(redis_a)).get();
        first.start(corrupt_values(redis_a)).get();
        bool wrong_type_failed{};
        try {
            (void)second.start(read(redis_b, tenant_a)).get();
        } catch (const std::exception&) {
            wrong_type_failed = true;
        }
        require(wrong_type_failed, "corrupt Redis data is an explicit cache failure for source fallback");
        first.start(clear(redis_a)).get();
        redis_b.close();
        bool disconnected_failed{};
        try {
            (void)second.start(read(redis_b, tenant_a)).get();
        } catch (const std::exception&) {
            disconnected_failed = true;
        }
        require(disconnected_failed, "disconnected Redis cannot masquerade as a valid snapshot");
        redis_a.close();
        loops.stop();
        loops.join();
        std::cout << "redis snapshot cache passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
