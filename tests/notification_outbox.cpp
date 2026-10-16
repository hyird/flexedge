#include "service/features/live_resource/notification_outbox.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/redis/redis_client.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include "service/config/schema.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/node_presence/notification_relay.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

namespace live = service::live_resource;
namespace outbox = live::outbox;
namespace cache = live::redis_snapshot_cache;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("notification outbox: " + std::string(message));
    }
}

struct delivery_context final {
    ruvia::db_client& database_;
    ruvia::redis_client* redis_;
    ruvia::stop_source stop_;
    std::size_t* database_calls_{};
    service::background::work_available::claim_signals claims_{};

    template <typename state_type>
    state_type& worker_state() {
        static_assert(std::is_same_v<state_type, service::background::work_available::claim_signals>);
        return claims_;
    }

    ruvia::db_handle db() const {
        if (database_calls_) {
            ++*database_calls_;
        }
        return database_.with_options({.stop_token_ = get_stop_token()});
    }
    ruvia::redis_handle redis() const {
        return redis_->handle();
    }
    const ruvia::worker_handle& worker() const {
        return database_.worker();
    }
    ruvia::stop_token get_stop_token() const {
        return stop_.token();
    }
};

ruvia::task<std::int64_t> row_count(ruvia::db_handle db) {
    const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_notification_outbox");
    co_return rows[0][0].as<std::int64_t>().value();
}

// The outbox participants of the rows this test commits.
ruvia::task<bool> relay_batch(delivery_context& context) {
    co_return co_await outbox::deliver_batch<service::node_presence::resource_relay, service::log_ingest::log_relay>(context);
}

ruvia::task<void> deliver(delivery_context& context, bool& delivered) {
    delivered = co_await relay_batch(context);
}

std::unordered_set<std::string> event_ids(const ruvia::redis_value& reply) {
    service::utils::require_redis_success(reply, "read delivered outbox events");
    require(reply.kind() == ruvia::redis_value::kind_type::array, "stream entries have an array reply");
    std::unordered_set<std::string> ids;
    for (const auto& entry : reply.array()) {
        require(entry.kind() == ruvia::redis_value::kind_type::array && entry.array().size() == 2,
            "delivered stream entry is well formed");
        const auto fields = entry.array()[1].array();
        bool found{};
        for (std::size_t index = 0; index + 1 < fields.size(); index += 2) {
            if (fields[index].string() == "event_id") {
                require(ids.insert(std::string(fields[index + 1].string())).second, "one durable event produces one stream entry");
                found = true;
            }
        }
        require(found, "every delivered entry carries its durable identity");
    }
    return ids;
}

ruvia::task<void> verify(ruvia::db_client& first, ruvia::db_client& second,
    ruvia::db_client& observer, ruvia::redis_client& redis, ruvia::redis_client& peer_redis,
    ruvia::redis_client& lost) {
    co_await first.connect();
    co_await second.connect();
    co_await observer.connect();
    co_await redis.connect();
    co_await peer_redis.connect();
    co_await lost.connect();
    auto db = first.handle();
    auto peer_db = second.handle();
    auto observer_db = observer.handle();
    const auto redis_handle = redis.handle();
    // Outbox mechanics are exercised with node log notifications: a small,
    // cache-neutral kind published to its own stream.
    const auto event_stream = std::string(service::log_ingest::notifications::stream_key);
    const std::vector<std::pair<std::string, std::string>> log_fields{
        {"tenant_id", "outbox-tenant"}, {"resource_type", "node"}, {"resource_id", "outbox-node"}};
    const auto log_payload = outbox::pack(log_fields);
    std::vector<std::string> created_ids;
    std::exception_ptr failure;
    try {
        const auto first_pid = co_await db.query("SELECT pg_backend_pid()");
        const auto second_pid = co_await peer_db.query("SELECT pg_backend_pid()");
        const auto pid_a = first_pid[0][0].as<std::int64_t>().value();
        const auto pid_b = second_pid[0][0].as<std::int64_t>().value();
        delivery_context context{.database_ = first, .redis_ = &redis, .stop_ = {}};
        delivery_context peer_context{.database_ = second, .redis_ = &peer_redis, .stop_ = {}};

        auto rollback = co_await db.begin_transaction();
        (void)co_await live::enqueue_notification(rollback,
            live::notifications::resource_notification("outbox-tenant").changed(live::resource::websites, "rolled-back"));
        const auto uncommitted_rows = co_await rollback.query("SELECT COUNT(*) FROM sys_notification_outbox");
        require(uncommitted_rows[0][0].as<int>() == 1,
            "notification is inserted inside the business transaction");
        co_await rollback.rollback();
        require(co_await row_count(db) == 0, "business rollback leaves no notification");

        const live::snapshot_key key{"outbox-tenant", live::resource::overview, {}, live::query_key("overview")};
        const auto before = co_await cache::read(redis_handle, key);
        const auto value = live::notifications::resource_notification("outbox-tenant").changed(live::resource::websites, "website");
        const auto fields = live::notifications::encode(value);
        const auto payload = outbox::pack(fields);
        auto transaction = co_await db.begin_transaction();
        const auto duplicate_id = co_await live::enqueue_notification(transaction, value);
        created_ids.push_back(duplicate_id);
        co_await transaction.commit();
        const auto stream_before = co_await redis_handle.command("XLEN", live::notifications::stream_key);
        co_await service::node_presence::resource_relay::publish(redis_handle, duplicate_id, payload);
        const auto after = co_await cache::read(redis_handle, key);
        require(before.generation_ != after.generation_, "publication invalidates the affected Redis projection");
        co_await service::node_presence::resource_relay::publish(redis_handle, duplicate_id, payload);
        const auto repeated = co_await cache::read(redis_handle, key);
        const auto stream_after = co_await redis_handle.command("XLEN", live::notifications::stream_key);
        require(after.generation_ == repeated.generation_, "retrying a published id does not invalidate again");
        require(stream_after.integer() == stream_before.integer() + 1, "retrying a published id does not duplicate its stream entry");
        require(co_await relay_batch(context), "durable retry claims the already-published event");
        require(co_await row_count(db) == 0, "retry removes the durable event after idempotent publication");
        require((co_await cache::read(redis_handle, key)).generation_ == repeated.generation_,
            "recovery after Redis success and before database deletion does not invalidate twice");

        auto invalid_presence = value;
        invalid_presence.node_seen("node|invalid");
        const auto invalid_id = service::utils::random_token();
        created_ids.push_back(invalid_id);
        const auto before_invalid = co_await redis_handle.command("XLEN", live::notifications::stream_key);
        bool rejected_presence{};
        try {
            const auto invalid_fields = live::notifications::encode(invalid_presence);
            co_await service::node_presence::resource_relay::publish(redis_handle, invalid_id, outbox::pack(invalid_fields));
        } catch (const std::exception&) {
            rejected_presence = true;
        }
        const auto after_invalid = co_await redis_handle.command("XLEN", live::notifications::stream_key);
        require(rejected_presence && before_invalid.integer() == after_invalid.integer(),
            "invalid presence metadata is rejected before appending any notification");
        require((co_await cache::read(redis_handle, key)).generation_ == repeated.generation_,
            "invalid publication cannot partially invalidate the cache");

        // The fast path has already committed when Redis fails. It must leave
        // the durable row; a background attempt records backoff, then recovers.
        lost.close();
        delivery_context failed_context{.database_ = first, .redis_ = &lost, .stop_ = {}};
        auto committed = co_await db.begin_transaction();
        co_await live::commit_and_notify(committed, failed_context, value);
        require(co_await row_count(db) == 1, "Redis failure preserves the committed outbox row");
        require(co_await relay_batch(failed_context), "failed delivery still claims durable work");
        const auto deferred = co_await db.query("SELECT id,attempts,available_at > NOW() FROM sys_notification_outbox");
        require(deferred.size() == 1 && deferred[0][1].as<int>() == 1 && deferred[0][2].as<bool>().value(),
            "failed delivery records attempts and bounded retry time");
        created_ids.emplace_back(deferred[0][0].value().value());
        require(!(co_await relay_batch(context)), "a deferred event is not reclaimed before it is due");
        (void)co_await db.execute("UPDATE sys_notification_outbox SET available_at = NOW()");
        require(co_await relay_batch(context), "restored Redis delivers due work");
        require(co_await row_count(db) == 0, "successful recovery acknowledges durable work");

        (void)co_await redis_handle.del(event_stream);
        auto batch = co_await db.begin_transaction();
        for (int index = 0; index != 64; ++index) {
            created_ids.push_back(co_await outbox::insert(batch, "log", log_payload));
        }
        co_await batch.commit();
        // Hold each claimant at its first DELETE after it has claimed its
        // own 32 rows. A missing SKIP LOCKED would instead leave the second
        // claimant waiting on tuple/transaction locks before it gets here.
        const auto advisory_key = static_cast<std::int64_t>(std::stoull(service::utils::random_token().substr(0, 15), nullptr, 16));
        (void)co_await db.execute("CREATE FUNCTION hold_outbox_delete() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN PERFORM pg_advisory_xact_lock(" +
                                  std::to_string(advisory_key) + "); RETURN NULL; END $$");
        (void)co_await db.execute("CREATE TRIGGER hold_outbox_delete AFTER DELETE ON sys_notification_outbox FOR EACH ROW EXECUTE FUNCTION hold_outbox_delete()");
        (void)co_await observer_db.query("SELECT pg_advisory_lock($1)", advisory_key);
        bool delivered_a{}, delivered_b{};
        ruvia::task_scope deliveries(first.worker());
        deliveries.spawn(deliver(context, delivered_a));
        deliveries.spawn(deliver(peer_context, delivered_b));
        bool both_claimed{};
        std::exception_ptr observation_failure;
        try {
            for (int attempt = 0; attempt != 50; ++attempt) {
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
                const auto states = co_await observer_db.query(
                    "SELECT state,wait_event_type,wait_event FROM pg_stat_activity WHERE pid IN ($1,$2)", pid_a, pid_b);
                require(states.size() == 2, "both independent database sessions remain observable");
                for (const auto& state : states) {
                    require(state[1].value() != "Lock" || state[2].value() == "advisory",
                        "concurrent delivery skips another claimant's locked rows");
                }
                both_claimed = states[0][2].value() == "advisory" && states[1][2].value() == "advisory";
                if (both_claimed) {
                    break;
                }
            }
        } catch (...) {
            observation_failure = std::current_exception();
        }
        (void)co_await observer_db.query("SELECT pg_advisory_unlock($1)", advisory_key);
        co_await deliveries.join();
        (void)co_await db.execute("DROP TRIGGER hold_outbox_delete ON sys_notification_outbox");
        (void)co_await db.execute("DROP FUNCTION hold_outbox_delete()");
        if (observation_failure) {
            std::rethrow_exception(observation_failure);
        }
        require(both_claimed && delivered_a && delivered_b, "both claimants progress while their peer holds row locks");
        const auto remaining = co_await db.query("SELECT COUNT(*),COALESCE(MAX(attempts),0) FROM sys_notification_outbox");
        require(remaining[0][0].as<int>() == 0,
            "two independent claimants finish all 64 events; remaining=" + std::string(remaining[0][0].value().value()) +
                ", max_attempts=" + std::string(remaining[0][1].value().value()));
        const auto entries = co_await redis_handle.command("XRANGE", event_stream, "-", "+");
        require(event_ids(entries).size() == 64 && entries.array().size() == 64,
            "concurrent delivery publishes each durable event exactly once");

        // Cancellation joins the worker's real timer and database attempt.
        std::size_t database_calls{};
        delivery_context stopping{.database_ = first, .redis_ = &redis, .stop_ = {}, .database_calls_ = &database_calls};
        ruvia::task_scope worker(first.worker());
        worker.spawn(outbox::run_worker<service::node_presence::resource_relay, service::log_ingest::log_relay>(stopping));
        for (int attempt = 0; attempt != 100 && database_calls < 2; ++attempt) {
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
        }
        const auto calls_after_catchup = database_calls;
        (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(1200));
        const bool idle_queries_stopped = calls_after_catchup >= 2 && database_calls == calls_after_catchup;
        std::exception_ptr scheduling_failure;
        try {
            auto future = co_await peer_db.begin_transaction();
            created_ids.push_back(co_await outbox::insert(future, "log", log_payload));
            (void)co_await future.execute("UPDATE sys_notification_outbox SET available_at = NOW() + INTERVAL '10 seconds'");
            co_await future.commit();
            // The committing worker wakes only its own claim loop.
            live::wake_own_outbox(stopping);
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(80));
            require(co_await row_count(peer_db) == 1, "future durable work is not delivered early");
            const auto scheduled_calls = database_calls;
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(120));
            require(database_calls == scheduled_calls, "a future retry waits for its deadline without database polling");
            (void)co_await peer_db.execute("UPDATE sys_notification_outbox SET available_at = NOW()");
            live::wake_own_outbox(stopping);
            bool released{};
            for (int attempt = 0; attempt != 100; ++attempt) {
                if (co_await row_count(peer_db) == 0) {
                    released = true;
                    break;
                }
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(10));
            }
            require(released, "a committed reschedule wakes the committer's own loop and replaces its previous future deadline");

            // Another worker can commit without waking this one, or its
            // committer can exit before waking its own loop. That work is
            // recovered within the explicit 15-second durable-claim fallback.
            (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(30));
            auto missed = co_await peer_db.begin_transaction();
            created_ids.push_back(co_await outbox::insert(missed, "log", log_payload));
            co_await missed.commit();
            const auto missed_at = std::chrono::steady_clock::now();
            bool recovered_missed{};
            for (int attempt = 0; attempt != 180; ++attempt) {
                if (co_await row_count(peer_db) == 0) {
                    recovered_missed = true;
                    break;
                }
                (void)co_await ruvia::sleep_for(first.worker(), std::chrono::milliseconds(100));
            }
            const auto fallback_elapsed = std::chrono::steady_clock::now() - missed_at;
            require(recovered_missed && fallback_elapsed >= std::chrono::seconds(14) && fallback_elapsed < std::chrono::seconds(18),
                "a missed wakeup is recovered by the bounded 15-second database claim fallback");
        } catch (...) {
            scheduling_failure = std::current_exception();
        }
        const auto stopped_at = std::chrono::steady_clock::now();
        stopping.stop_.request_stop();
        co_await worker.join();
        if (scheduling_failure) {
            std::rethrow_exception(scheduling_failure);
        }
        require(idle_queries_stopped, "an idle outbox performs no empty scan before its 15-second fallback");
        require(std::chrono::steady_clock::now() - stopped_at < std::chrono::milliseconds(500),
            "worker cancellation promptly joins its owned operations");
        std::cout << "rollback, durable retry, atomic idempotence, generation fencing, two-client SKIP LOCKED, own-loop wakeup, 15-second missed-hint recovery and cancellation passed\n";
    } catch (...) {
        failure = std::current_exception();
    }
    for (const auto& id : created_ids) {
        (void)co_await redis_handle.del("flexedge:notification-published:v1:" + id);
    }
    (void)co_await redis_handle.del(event_stream);
    if (failure) {
        std::rethrow_exception(failure);
    }
}

}  // namespace

int main() {
    try {
        const auto postgres = test_support::isolated_postgres_config();
        const auto redis_config = test_support::isolated_redis_config();
        if (!postgres || !redis_config) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*postgres);
        const ruvia::db_migrator migrator(temporary_database.config(), {.table_ = "schema_migrations"});
        (void)migrator.migrate(service::config::schema_migrations);
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        ruvia::db_client first(loop, temporary_database.config()), second(loop, temporary_database.config()),
            observer(loop, temporary_database.config());
        ruvia::redis_client redis(loop, *redis_config), peer_redis(loop, *redis_config), lost(loop, *redis_config);
        loops.start();
        std::exception_ptr failure;
        try {
            loop.start(verify(first, second, observer, redis, peer_redis, lost)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            loop.start(first.shutdown()).get();
            loop.start(second.shutdown()).get();
            loop.start(observer.shutdown()).get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        first.close();
        second.close();
        observer.close();
        redis.close();
        peer_redis.close();
        lost.close();
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
