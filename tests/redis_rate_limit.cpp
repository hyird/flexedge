#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/redis/redis_client.h>

#include <array>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "service/features/outbound_http/quota.h"
#include "service/features/rate_limit/store.h"
#include "support/redis_test.h"

namespace {

using namespace std::chrono_literals;
namespace rates = service::rate_limit;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("redis rate limit: ") + message);
    }
}

ruvia::task<std::size_t> consume_many(ruvia::redis_client& client, const std::vector<rates::bucket>& buckets) {
    std::size_t granted{};
    for (int index = 0; index < 60; ++index) {
        const auto result = co_await rates::try_consume(client.handle(), buckets);
        granted += result.allowed_ ? 1 : 0;
    }
    co_return granted;
}

ruvia::task<void> cleanup(ruvia::redis_client& client, const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        (void)co_await client.handle().del(key);
    }
}

ruvia::task<void> verify_account_isolation(ruvia::redis_client& client, std::string prefix,
    std::vector<std::string>& keys) {
    const auto global = prefix + "global";
    const auto account = prefix + "account-a";
    const auto other = prefix + "account-b";
    keys.insert(keys.end(), {global, account, other});
    std::array first{rates::bucket{global, {3, 600s}}, rates::bucket{account, {1, 600s}}};
    require((co_await rates::try_consume(client.handle(), first)).allowed_, "first account is admitted");
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto denied = co_await rates::try_consume(client.handle(), first);
        require(!denied.allowed_ && denied.retry_after_ > 0ms, "exhausted account is rejected with retry delay");
    }
    const std::array second{rates::bucket{global, {3, 600s}}, rates::bucket{other, {3, 600s}}};
    require((co_await rates::try_consume(client.handle(), second)).allowed_, "denials did not consume global tokens");
    require((co_await rates::try_consume(client.handle(), second)).allowed_, "remaining global token is admitted");
    require(!(co_await rates::try_consume(client.handle(), second)).allowed_, "global cap applies across accounts");
}

ruvia::task<void> verify_refill_and_expiry(ruvia::redis_client& client, std::string key) {
    const std::array buckets{rates::bucket{key, {1, 200ms}}};
    require((co_await rates::try_consume(client.handle(), buckets)).allowed_, "fresh short bucket is admitted");
    const auto denied = co_await rates::try_consume(client.handle(), buckets);
    require(!denied.allowed_ && denied.retry_after_ > 0ms && denied.retry_after_ <= 200ms,
        "retry delay is bounded by refill interval");
    const auto ttl = co_await client.handle().pttl(key);
    require(ttl.state() == ruvia::redis_ttl_state::expiring && ttl.remaining() && *ttl.remaining() <= 201ms,
        "bucket always carries a bounded TTL");
    (void)co_await ruvia::sleep_for(client.worker(), 260ms);
    require(!(co_await client.handle().exists(key)), "idle bucket expires");
    co_await rates::acquire(client.handle(), client.worker(), buckets, 500ms, {});
    co_await rates::acquire(client.handle(), client.worker(), buckets, 500ms, {});
    (void)co_await client.handle().del(key);
}

ruvia::task<void> verify_faults(ruvia::redis_client& client, ruvia::redis_client& lost_client,
    std::string key) {
    const auto lost = lost_client.handle();
    lost_client.close();
    const std::array buckets{rates::bucket{key, {1, 600s}}};
    bool unavailable = false;
    try {
        (void)co_await rates::try_consume(lost, buckets);
    } catch (const rates::unavailable&) {
        unavailable = true;
    }
    require(unavailable, "Redis transport failure fails closed");
    require(!(co_await client.handle().exists(key)), "failed acquisition cannot create quota state");
    (void)co_await client.handle().hset(key, "tokens", "invalid");
    unavailable = false;
    try {
        (void)co_await rates::try_consume(client.handle(), buckets);
    } catch (const rates::unavailable&) {
        unavailable = true;
    }
    require(unavailable, "malformed Redis state fails closed");
    (void)co_await client.handle().del(key);
}

ruvia::task<bool> verify_cancel(ruvia::redis_client& client, std::string key, ruvia::stop_token stop) {
    const std::array buckets{rates::bucket{key, {1, 2s}}};
    require((co_await rates::try_consume(client.handle(), buckets)).allowed_, "cancel test consumes its initial token");
    try {
        co_await rates::acquire(client.handle(), client.worker(), buckets, 5s, stop);
    } catch (const rates::cancelled&) {
        co_return true;
    }
    co_return false;
}

ruvia::task<void> verify_wait_bound(ruvia::redis_client& client, std::string key) {
    const std::array buckets{rates::bucket{key, {1, 600s}}};
    (void)co_await rates::try_consume(client.handle(), buckets);
    bool exhausted = false;
    try {
        co_await rates::acquire(client.handle(), client.worker(), buckets, 40ms, {});
    } catch (const rates::exhausted&) {
        exhausted = true;
    }
    require(exhausted, "quota wait has a strict upper bound");
    (void)co_await client.handle().del(key);
}

void verify_scope_keys() {
    const service::config::rate_limit_config config;
    const auto first = service::outbound_http::quota_buckets("cloudflare",
        {.tenant_id_ = "tenant-a", .account_id_ = "private-account"}, config);
    const auto second = service::outbound_http::quota_buckets("cloudflare",
        {.tenant_id_ = "tenant-b", .account_id_ = "private-account"}, config);
    const auto third = service::outbound_http::quota_buckets("aliyun-dns",
        {.tenant_id_ = "tenant-a", .account_id_ = "private-account"}, config);
    require(first[0].key_ == second[0].key_, "origin-wide budget is shared across tenants");
    require(first[1].key_ != second[1].key_ && first[1].key_ != third[1].key_,
        "account namespace includes tenant and provider origin");
    require(first[1].key_.find("private-account") == std::string::npos,
        "account identifiers never appear in Redis keys");
}

}  // namespace

int main() {
    std::optional<ruvia::redis_config> config;
    try {
        config = test_support::isolated_redis_config();
        verify_scope_keys();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (!config) {
        return 77;
    }
    try {
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto first = loops.loop(0);
        const auto second = loops.loop(1);
        ruvia::redis_client left(first, *config);
        ruvia::redis_client right(second, *config);
        ruvia::redis_client lost(first, *config);
        loops.start();
        first.start(left.connect()).get();
        second.start(right.connect()).get();
        first.start(lost.connect()).get();
        const auto prefix = "flexedge:test:rate:{" + service::utils::random_token() + "}:";
        std::vector<std::string> keys{prefix + "atomic", prefix + "cancel"};
        const std::vector<rates::bucket> atomic{rates::bucket{keys[0], {10, 600s}}};
        auto a = first.start(consume_many(left, atomic));
        auto b = second.start(consume_many(right, atomic));
        require(a.get() + b.get() == 10, "two independent worker clients share one atomic quota");
        first.start(verify_account_isolation(left, prefix, keys)).get();
        first.start(verify_refill_and_expiry(left, prefix + "expiry")).get();
        first.start(verify_faults(left, lost, prefix + "fault")).get();
        first.start(verify_wait_bound(left, prefix + "bound")).get();
        ruvia::stop_source stop;
        auto cancellation = first.start(verify_cancel(left, keys[1], stop.token()));
        std::this_thread::sleep_for(50ms);
        const auto cancelled_at = std::chrono::steady_clock::now();
        stop.request_stop();
        require(cancellation.get(), "quota wait observes owner cancellation");
        require(std::chrono::steady_clock::now() - cancelled_at < 1s, "cancelled wait ends promptly");
        first.start(cleanup(left, keys)).get();
        first.start(left.shutdown()).get();
        second.start(right.shutdown()).get();
        first.start(lost.shutdown()).get();
        loops.stop();
        loops.join();
        std::cout << "Redis shared quota: atomic clients, account isolation, refill/TTL, cancellation and fail-closed checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
