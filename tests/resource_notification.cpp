#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>
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
#include "support/redis_test.h"

namespace {

namespace live = service::live_resource;
namespace notifications = service::live_resource::notifications;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("resource notification: ") + message);
    }
}

// One Web Worker's delivery state, as its reader sees it.
struct worker_states final {
    live::hub live_;
    live::node_deadlines deadlines_;
    service::node_dispatch::fanout::hub dispatch_;

    template <typename state_type>
    state_type& worker_state() {
        if constexpr (std::is_same_v<state_type, live::hub>) {
            return live_;
        } else if constexpr (std::is_same_v<state_type, live::node_deadlines>) {
            return deadlines_;
        } else {
            return dispatch_;
        }
    }
};

ruvia::task<bool> signalled(live::hub::subscription& subscription, std::chrono::milliseconds timeout) {
    co_return (co_await subscription.receive_for(timeout, {})).has_value();
}

ruvia::task<std::vector<notifications::entry>> exchange(ruvia::redis_client& redis, ruvia::redis_client& lost) {
    const auto handle = redis.handle();
    (void)co_await handle.command("DEL", notifications::stream_key);
    const auto cursor = co_await notifications::current_cursor(handle);
    notifications::sender sender;
    // Producers append only after their commit; a Redis connection that fails
    // never fails the caller, it leaves the tenant owed one recovery.
    const auto lost_handle = lost.handle();
    lost.close();
    co_await sender.send(lost_handle,
        std::move(notifications::resource_notification("tenant-a").changed(live::resource::websites, "lost")));
    notifications::resource_notification attempt("tenant-a");
    attempt.task_attempt("task-a", {.subtask_id_ = "zone-a", .event_id_ = 42})
        .changed(live::resource::dns_zones, "zone-a");
    co_await sender.send(handle, attempt);
    co_await sender.send(handle,
        std::move(notifications::resource_notification("tenant-b").changed(live::resource::nodes, "node-b")));
    auto batch = co_await notifications::read(handle, cursor);
    (void)co_await handle.command("DEL", notifications::stream_key);
    co_return std::move(batch.entries_);
}

// A producer worker whose Redis answers again after its failed send.
struct producer_context final {
    ruvia::redis_client* redis_;

    ruvia::redis_handle redis() const {
        return redis_->handle();
    }
    const ruvia::worker_handle& worker() const {
        return redis_->worker();
    }
};

// A failed send followed by no further sends: the producer's own retry still
// appends the owed recovery once Redis is back.
ruvia::task<std::vector<notifications::entry>> owed_without_further_sends(ruvia::redis_client& redis,
    ruvia::redis_client& lost) {
    const auto handle = redis.handle();
    (void)co_await handle.command("DEL", notifications::stream_key);
    const auto cursor = co_await notifications::current_cursor(handle);
    notifications::sender sender;
    producer_context context{.redis_ = &redis};
    ruvia::task_scope retries(redis.worker());
    retries.spawn(sender.retry_owed(context, retries.get_stop_token()));
    const auto lost_handle = lost.handle();
    lost.close();
    co_await sender.send(lost_handle,
        std::move(notifications::resource_notification("tenant-c").changed(live::resource::websites, "lost")));
    const bool owed = sender.owes();
    for (int wait = 0; wait < 50 && sender.owes(); ++wait) {
        (void)co_await ruvia::sleep_for(redis.worker(), std::chrono::milliseconds(100), {});
    }
    const bool settled = !sender.owes();
    retries.request_stop();
    co_await retries.join();
    require(owed && settled, "the owed recovery is retried without a further send");
    auto batch = co_await notifications::read(handle, cursor);
    (void)co_await handle.command("DEL", notifications::stream_key);
    co_return std::move(batch.entries_);
}

}  // namespace

int main() {
    std::optional<ruvia::redis_config> config;
    try {
        config = test_support::isolated_redis_config();
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
        ruvia::redis_client redis(first, *config);
        ruvia::redis_client lost(first, *config);
        ruvia::redis_client lost_again(first, *config);
        loops.start();
        first.start(redis.connect()).get();
        first.start(lost.connect()).get();
        first.start(lost_again.connect()).get();
        const auto entries = first.start(exchange(redis, lost)).get();
        // Exactly: the owed recovery, then the attempt, then the other tenant.
        std::optional<std::size_t> owed;
        std::optional<std::size_t> attempted;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& value = entries[index].notification_;
            require(value.has_value(), "every entry decodes");
            if (value->missed_until_ && !owed) {
                owed = index;
            }
            if (!value->task_attempts_.empty()) {
                attempted = index;
            }
        }
        require(owed && attempted && *owed < *attempted && entries[*owed].notification_->tenant_id_ == "tenant-a" &&
                    entries[*owed].notification_->changes_.empty(),
            "the failed send is owed as a recovery before the next notification");
        require(entries.back().notification_->tenant_id_ == "tenant-b", "later sends continue");
        const auto& attempt = *entries[*attempted].notification_;
        require(attempt.task_attempts_.size() == 1 && attempt.task_attempts_[0].task_id_ == "task-a" &&
                    attempt.task_attempts_[0].notice_.event_id_ == 42 &&
                    attempt.task_attempts_[0].notice_.subtask_id_ == "zone-a",
            "attempt notice round-trips through Redis");
        const auto& owed_entry = *entries[*owed].notification_;

        // Each worker delivers to its own subscriptions only.
        worker_states worker_a;
        worker_states worker_b;
        std::vector<live::subscription_part> parts;
        parts.push_back({live::resource::operations, "task-a", live::query_key("history", "zone-a"), true});
        auto history_a = worker_a.live_.subscribe(first.handle(), "tenant-a", std::move(parts));
        auto history_b = worker_b.live_.subscribe(second.handle(), "tenant-a", live::resource::operations, "task-a");
        auto other_tenant = worker_a.live_.subscribe(first.handle(), "tenant-b", live::resource::operations, "task-a");
        live::deliver_notification(worker_a, attempt, std::nullopt);
        require(first.start(signalled(history_a, std::chrono::seconds(1))).get(), "own subscription woken");
        const auto pending = history_a.drain();
        require(pending.task_attempts_.size() == 1 && pending.task_attempts_[0].event_id_ == 42 &&
                    !pending.history_reset_,
            "incremental part receives the notice");
        require(!second.start(signalled(history_b, std::chrono::milliseconds(50))).get(),
            "another worker's subscription is untouched");
        require(!first.start(signalled(other_tenant, std::chrono::milliseconds(50))).get(), "tenant isolation");

        // An owed recovery newer than the reader's own recovery resends
        // snapshots of that tenant; an older one is already covered.
        live::deliver_notification(worker_a, owed_entry, *owed_entry.missed_until_ + 1);
        require(!first.start(signalled(history_a, std::chrono::milliseconds(50))).get(),
            "recovery older than the reader's own is skipped");
        live::deliver_notification(worker_a, owed_entry, std::nullopt);
        require(first.start(signalled(history_a, std::chrono::seconds(1))).get(), "owed recovery wakes the tenant");
        const auto recovered = history_a.drain();
        require(recovered.snapshot_ && recovered.history_reset_ && recovered.recovery_ &&
                    recovered.task_attempts_.empty(),
            "incremental part resends its snapshot");
        require(!first.start(signalled(other_tenant, std::chrono::milliseconds(50))).get(),
            "recovery stays within the tenant");

        const auto retried = first.start(owed_without_further_sends(redis, lost_again)).get();
        require(retried.size() == 1 && retried[0].notification_ && retried[0].notification_->tenant_id_ == "tenant-c" &&
                    retried[0].notification_->missed_until_ && retried[0].notification_->changes_.empty(),
            "exactly the owed recovery entry is appended");

        first.start(redis.shutdown()).get();
        first.start(lost.shutdown()).get();
        first.start(lost_again.shutdown()).get();
        loops.stop();
        loops.join();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Resource notifications: Redis round trip, owed recovery, per-worker delivery passed\n";
    return 0;
}
