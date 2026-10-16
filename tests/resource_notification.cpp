#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_wait_result.h>
#include <ruvia/web/redis/redis_client.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "service/features/live_resource/notification_reader.h"
#include "service/features/node_presence/notification_relay.h"
#include "support/redis_test.h"

namespace {

namespace live = service::live_resource;
namespace notifications = live::notifications;
namespace cache = live::redis_snapshot_cache;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("resource notification: ") + message);
    }
}

struct worker_states final {
    live::hub live_;

    template <typename state_type>
    state_type& worker_state() {
        static_assert(std::is_same_v<state_type, live::hub>);
        return live_;
    }
};

ruvia::task<bool> signalled(live::hub::subscription& subscription, std::chrono::milliseconds timeout) {
    co_return (co_await subscription.receive_for(timeout, {})).has_value();
}

ruvia::task<std::vector<notifications::entry>> exchange(ruvia::redis_client& redis) {
    const auto handle = redis.handle();
    constexpr std::string_view first_id{"resource-notification-qa-first"};
    constexpr std::string_view second_id{"resource-notification-qa-second"};
    constexpr std::string_view failed_id{"resource-notification-qa-failed"};
    const auto dedup_key = [](std::string_view id) { return "flexedge:notification-published:v1:" + std::string(id); };
    const auto first_key = dedup_key(first_id), second_key = dedup_key(second_id), failed_key = dedup_key(failed_id);
    (void)co_await handle.command("DEL", notifications::stream_key, first_key, second_key, failed_key);
    for (const auto key : cache::keys) {
        (void)co_await handle.command("DEL", key);
    }
    const auto cursor = co_await notifications::current_cursor(handle);

    const live::snapshot_key tenant_a{"tenant-a", live::resource::overview, "", live::query_key("overview")};
    const live::snapshot_key tenant_b{"tenant-b", live::resource::overview, "", live::query_key("overview")};
    const auto before_a = co_await cache::read(handle, tenant_a);
    const auto before_b = co_await cache::read(handle, tenant_b);
    require(co_await cache::store(handle, tenant_a, before_a.generation_, {"cached-a", false}), "first cache primed");
    require(co_await cache::store(handle, tenant_b, before_b.generation_, {"cached-b", false}), "other tenant cache primed");

    notifications::resource_notification attempt("tenant-a");
    attempt.task_attempt("task-a", 42).changed(live::resource::tasks, "task-a").changed(live::resource::dns_zones, "zone-a");
    const auto fields = notifications::encode(attempt);
    const auto encoded = [&](std::string_view name) {
        for (const auto& [field, value] : fields) {
            if (field == name) {
                return value;
            }
        }
        return std::string{};
    };
    require(encoded("task_attempts") == "task-a,42" && encoded("changes") == "tasks:task-a;dns_zones:zone-a",
        "a task attempt travels as its task id and event id; the task list resource is named tasks");
    const auto payload = live::outbox::pack(fields);
    co_await service::node_presence::resource_relay::publish(handle, first_id, payload);
    const auto after_a = co_await cache::read(handle, tenant_a);
    const auto after_b = co_await cache::read(handle, tenant_b);
    require(!after_a.snapshot_ && after_a.generation_ != before_a.generation_,
        "the commit notification atomically invalidates the affected tenant projection");
    require(after_b.snapshot_ && after_b.snapshot_->data_ == "cached-b", "another tenant's projection remains cached");
    require(co_await cache::store(handle, tenant_a, after_a.generation_, {"refreshed-a", false}), "updated projection cached");
    co_await service::node_presence::resource_relay::publish(handle, first_id, payload);
    require((co_await cache::read(handle, tenant_a)).snapshot_.has_value(),
        "replaying an already published event does not invalidate a newer projection");
    require(!co_await cache::store(handle, tenant_a, before_a.generation_, {"stale-a", false}),
        "a query started before the commit cannot repopulate stale data");

    const auto other = notifications::encode(
        std::move(notifications::resource_notification("tenant-b").changed(live::resource::nodes, "node-b")));
    co_await service::node_presence::resource_relay::publish(handle, second_id, live::outbox::pack(other));
    const auto first_reader = co_await notifications::read(handle, cursor);
    const auto second_reader = co_await notifications::read(handle, cursor);
    require(first_reader.entries_.size() == 2 && second_reader.entries_.size() == 2 &&
                first_reader.entries_.front().id_ == second_reader.entries_.front().id_ &&
                first_reader.entries_.back().id_ == second_reader.entries_.back().id_,
        "two independent Web Worker cursors each receive the broadcast exactly once");

    (void)co_await handle.command("DEL", notifications::stream_key);
    (void)co_await handle.command("SET", notifications::stream_key, "wrong-type-fixture");
    bool rejected{};
    try {
        co_await service::node_presence::resource_relay::publish(handle, failed_id, payload);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && (co_await handle.command("GET", failed_key)).null(),
        "failed publication does not falsely record successful deduplication");
    (void)co_await handle.command("DEL", notifications::stream_key);
    co_await service::node_presence::resource_relay::publish(handle, failed_id, payload);
    require((co_await notifications::read(handle, "0-0")).entries_.size() == 1,
        "the persisted event can be published after repairing Redis");
    (void)co_await handle.command("DEL", notifications::stream_key, first_key, second_key, failed_key);
    for (const auto key : cache::keys) {
        (void)co_await handle.command("DEL", key);
    }
    co_return first_reader.entries_;
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto first = loops.loop(0), second = loops.loop(1);
        ruvia::redis_client redis(first, *config);
        loops.start();
        first.start(redis.connect()).get();
        const auto entries = first.start(exchange(redis)).get();
        require(entries.size() == 2 && entries.front().notification_ && entries.back().notification_, "all event fields decode");
        const auto& attempt = *entries.front().notification_;
        require(attempt.task_attempts_.size() == 1 && attempt.task_attempts_[0].task_id_ == "task-a" &&
                    attempt.task_attempts_[0].notice_.event_id_ == 42 && attempt.changes_.size() == 2 &&
                    attempt.changes_[0].resource_ == live::resource::tasks && attempt.changes_[0].id_ == "task-a" &&
                    entries.back().notification_->tenant_id_ == "tenant-b",
            "task attempt and tenant scope round trip");

        worker_states worker_a, worker_b;
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::tasks, "task-a", live::query_key("history"), true});
        auto history_a = worker_a.live_.subscribe(first.handle(), "tenant-a", std::move(parts));
        auto history_b = worker_b.live_.subscribe(second.handle(), "tenant-a", live::resource::tasks, "task-a");
        auto other_tenant = worker_a.live_.subscribe(first.handle(), "tenant-b", live::resource::tasks, "task-a");
        live::resource_subscriptions::deliver(worker_a, attempt, std::nullopt);
        require(first.start(signalled(history_a, std::chrono::seconds(1))).get(), "own subscription is woken");
        const auto pending = history_a.drain();
        require(pending.task_attempts_.size() == 1 && pending.task_attempts_[0].event_id_ == 42 && !pending.history_reset_,
            "the incremental subscription receives the committed attempt");
        require(!second.start(signalled(history_b, std::chrono::milliseconds(50))).get(),
            "another worker's subscription is untouched until its own reader delivers");
        require(!first.start(signalled(other_tenant, std::chrono::milliseconds(50))).get(), "tenant isolation");
        live::resource_subscriptions::deliver(worker_b, attempt, std::nullopt);
        require(second.start(signalled(history_b, std::chrono::seconds(1))).get(),
            "the second reader delivers to its own subscription");
        first.start(redis.shutdown()).get();
        loops.stop();
        loops.join();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Resource notifications: Redis atomic invalidation, idempotent publish, broadcast, recovery passed\n";
    return 0;
}
