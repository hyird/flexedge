#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_schema.h>
#include <ruvia/web/redis/redis_client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#include "service/features/node_presence/notification_relay.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

using namespace std::chrono_literals;
namespace presence = service::node_presence;
namespace notifications = service::live_resource::notifications;
namespace cache = service::live_resource::redis_snapshot_cache;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("node presence: ") + message);
    }
}

ruvia::task<std::int64_t> redis_now(ruvia::redis_handle redis) {
    const auto clock = co_await redis.command("TIME");
    const auto parts = clock.array();
    co_return std::stoll(std::string(parts[0].string())) * 1000 + std::stoll(std::string(parts[1].string())) / 1000;
}

ruvia::task<void> reset(ruvia::redis_client& client) {
    for (const auto key : {presence::deadlines_key, presence::generation_key, presence::seeded_generation_key,
             notifications::stream_key, cache::generation_keys[0], cache::generation_keys[1], cache::generation_keys[2]}) {
        (void)co_await client.handle().del(key);
    }
}

ruvia::task<presence::due_node> prepare_expiry(ruvia::redis_client& client) {
    const auto redis = client.handle();
    const auto now = co_await redis_now(redis);
    co_await presence::record(redis, "tenant-a", "node-a", now - 15000 + 120);
    co_await presence::record(redis, "tenant-a", "node-a", now - 15000 - 1);
    require((co_await presence::inspect(redis)).due_.empty(), "old event cannot expire a fresher node");
    (void)co_await ruvia::sleep_for(client.worker(), 150ms);
    const auto candidates = co_await presence::inspect(redis);
    require(candidates.due_.size() == 1, "one real online window expires");
    co_return candidates.due_.front();
}

ruvia::task<bool> expire(ruvia::redis_client& client, const presence::due_node& candidate) {
    co_return co_await presence::expire(client.handle(), candidate);
}

ruvia::task<void> verify_expired(ruvia::redis_client& client, const presence::due_node& candidate) {
    const auto redis = client.handle();
    const auto stream = co_await redis.command("XLEN", notifications::stream_key);
    require(stream.integer() == 1, "concurrent workers publish exactly one offline transition");
    require(!(co_await redis.exists(presence::deadlines_key)), "completed expiry has no lingering deadline");
    require((co_await redis.hget(cache::generation_keys[0], cache::resource_scope("tenant-a", service::live_resource::resource::overview))).has_value(),
        "offline transition invalidates cached overview in the same script");
    co_await presence::record(redis, "tenant-a", "node-a", (co_await redis_now(redis)) - 16000);
    require(!(co_await redis.exists(presence::deadlines_key)), "delayed event cannot resurrect an expired node");
    require(!co_await presence::expire(redis, candidate), "same claimed expiry is idempotent");
}

ruvia::task<void> verify_renewal_and_faults(ruvia::redis_client& client) {
    const auto redis = client.handle();
    auto candidate = co_await prepare_expiry(client);
    co_await presence::record(redis, "tenant-a", "node-a", co_await redis_now(redis));
    require(!co_await presence::expire(redis, candidate), "a heartbeat arriving after candidate selection wins");
    (void)co_await redis.del(presence::deadlines_key);
    candidate = co_await prepare_expiry(client);
    (void)co_await redis.del(notifications::stream_key);
    (void)co_await redis.set(notifications::stream_key, "wrong-type");
    bool failed = false;
    try {
        (void)co_await presence::expire(redis, candidate);
    } catch (const std::exception&) {
        failed = true;
    }
    require(failed && (co_await presence::inspect(redis)).due_.size() == 1,
        "publication failure leaves the expiry claimable in Redis");
    (void)co_await redis.del(notifications::stream_key);
    require(co_await presence::expire(redis, candidate), "expiry resumes after publication recovers");
}

ruvia::task<void> verify_publication_replay(ruvia::redis_client& client) {
    co_await reset(client);
    const auto redis = client.handle();
    const auto id = service::utils::random_token();
    const auto dedup_key = "flexedge:notification-published:v1:" + id;
    notifications::resource_notification value("tenant-a");
    value.node_seen("node-a").changed(service::live_resource::resource::nodes, "node-a");
    value.node_seen_unix_ms_ = co_await redis_now(redis);
    const auto payload = service::live_resource::outbox::pack(notifications::encode(value));
    // Node changes invalidate the cached overview; node lists are not cached.
    const auto scope = cache::resource_scope("tenant-a", service::live_resource::resource::overview);
    (void)co_await redis.set(presence::deadlines_key, "wrong-type");
    bool rejected{};
    try {
        co_await presence::resource_relay::publish(redis, id, payload);
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && !(co_await redis.exists(notifications::stream_key)) &&
                !(co_await redis.exists(dedup_key)) && !(co_await redis.hget(cache::generation_keys[0], scope)),
        "bad presence storage cannot partially append, invalidate or acknowledge an event");
    (void)co_await redis.del(presence::deadlines_key);
    co_await presence::resource_relay::publish(redis, id, payload);
    const auto deadline = co_await redis.command("ZSCORE", presence::deadlines_key, "tenant-a|node-a");
    const auto generation = co_await redis.hget(cache::generation_keys[0], scope);
    require(!deadline.null() && generation && (co_await redis.command("XLEN", notifications::stream_key)).integer() == 1 &&
                (co_await redis.exists(dedup_key)),
        "a repaired publication commits its heartbeat, event, cache invalidation and replay fence together");
    co_await presence::record(redis, "tenant-a", "node-a", value.node_seen_unix_ms_ + 1000);
    const auto renewed = co_await redis.command("ZSCORE", presence::deadlines_key, "tenant-a|node-a");
    co_await presence::resource_relay::publish(redis, id, payload);
    const auto repeated = co_await redis.command("ZSCORE", presence::deadlines_key, "tenant-a|node-a");
    require(renewed.string() == repeated.string() &&
                (co_await redis.hget(cache::generation_keys[0], scope)) == generation &&
                (co_await redis.command("XLEN", notifications::stream_key)).integer() == 1,
        "replaying a committed event preserves the newer deadline without duplicate invalidation or append");
    (void)co_await redis.del(presence::deadlines_key);
    value.node_seen_unix_ms_ = (co_await redis_now(redis)) - 16000;
    const auto delayed_id = service::utils::random_token();
    co_await presence::resource_relay::publish(redis, delayed_id,
        service::live_resource::outbox::pack(notifications::encode(value)));
    require(!(co_await redis.exists(presence::deadlines_key)),
        "a delayed committed heartbeat cannot resurrect presence after expiry or Redis reset");
    (void)co_await redis.del(dedup_key);
    (void)co_await redis.del("flexedge:notification-published:v1:" + delayed_id);
    co_await reset(client);
}

ruvia::task<void> verify_generation(ruvia::redis_client& client) {
    const auto redis = client.handle();
    (void)co_await redis.del(notifications::stream_key);
    const auto first = co_await presence::inspect(redis);
    require(co_await presence::complete_seed(redis, first.generation_), "first recovery completes");
    const auto epoch = co_await redis.hget(cache::generation_keys[0], "*");
    require(epoch.has_value(), "recovery invalidates every cached tenant projection");
    require(co_await presence::complete_seed(redis, first.generation_), "same generation can finish on every worker");
    require((co_await redis.command("XLEN", notifications::stream_key)).integer() == 1,
        "same generation emits only one recovery notification");
    (void)co_await redis.del(presence::generation_key);
    const auto next = co_await presence::inspect(redis);
    require(next.generation_ != first.generation_, "a missing Redis generation is detected without a transport error");
    require(!co_await presence::complete_seed(redis, first.generation_), "old seed cannot complete a newer Redis generation");
    require(co_await presence::complete_seed(redis, next.generation_), "new generation completes");
    const auto next_epoch = co_await redis.hget(cache::generation_keys[0], "*");
    require(next_epoch && *next_epoch != *epoch, "recovery uses a new cache nonce");
    require((co_await redis.command("XLEN", notifications::stream_key)).integer() == 2,
        "a changed Redis generation emits one fresh recovery");
}

struct seed_context final {
    ruvia::db_client* database_;
    ruvia::redis_client* redis_;

    ruvia::db_handle db() const {
        return database_->handle();
    }
    ruvia::redis_handle redis() const {
        return redis_->handle();
    }
    ruvia::stop_token get_stop_token() const {
        return {};
    }
};

ruvia::task<void> verify_database_seed(ruvia::db_client& database, ruvia::redis_client& client) {
    auto db = database.handle();
    ruvia::db_schema schema({.driver_ = ruvia::db_driver::postgresql});
    schema.create_table({.name_ = "sys_node", .columns_ = {{.name_ = "id", .type_ = {.data_type_ = ruvia::db_data_type::uuid}, .primary_key_ = true}, {.name_ = "tenant_id", .type_ = {.data_type_ = ruvia::db_data_type::uuid}}, {.name_ = "last_heartbeat_at", .type_ = {.data_type_ = ruvia::db_data_type::timestamp_tz}, .nullable_ = true}, {.name_ = "registration_status", .type_ = {.data_type_ = ruvia::db_data_type::text}}, {.name_ = "status", .type_ = {.data_type_ = ruvia::db_data_type::text}}, {.name_ = "deleted_at", .type_ = {.data_type_ = ruvia::db_data_type::timestamp_tz}, .nullable_ = true}}});
    for (const auto& migration : schema.compile("presence_fixture")) {
        (void)co_await db.execute(migration.sql());
    }
    const std::string tenant = "00000000-0000-0000-0000-000000000001";
    const std::array<std::string, 4> ids{
        "00000000-0000-0000-0000-000000000010", "00000000-0000-0000-0000-000000000011",
        "00000000-0000-0000-0000-000000000012", "00000000-0000-0000-0000-000000000013"};
    for (std::size_t index = 0; index < ids.size(); ++index) {
        ruvia::db_query insert;
        insert.insert_into("sys_node", {"id", "tenant_id", "last_heartbeat_at", "registration_status", "status", "deleted_at"})
            .values({insert.value(ids[index]), insert.value(tenant), insert.call("now"),
                insert.value("registered"), insert.value(index == 1 ? "disabled" : "enabled"),
                index == 2 ? insert.call("now") : insert.value(nullptr)});
        (void)co_await db.execute(insert);
    }
    ruvia::db_query stale;
    stale.update("sys_node").set("last_heartbeat_at", stale.binary(stale.call("now"), ruvia::db_binary_operator::subtract, stale.cast(stale.value("1 hour"), ruvia::db_data_type::interval))).where(stale.binary(stale.column("id"), ruvia::db_binary_operator::equal, stale.value(ids[3])));
    (void)co_await db.execute(stale);
    (void)co_await client.handle().del(presence::deadlines_key);
    const auto inspection = co_await presence::inspect(client.handle());
    seed_context context{.database_ = &database, .redis_ = &client};
    require(co_await presence::seed_from_database(context, inspection.generation_), "ORM seed executes against PostgreSQL");
    const auto members = co_await client.handle().command("ZRANGE", presence::deadlines_key, "0", "-1");
    require(members.array().size() == 1 && members.array()[0].string() == tenant + "|" + ids[0],
        "seed includes only registered enabled undeleted recent nodes");
    const auto score_before = co_await client.handle().command("ZSCORE", presence::deadlines_key, tenant + "|" + ids[0]);
    co_await presence::record(client.handle(), tenant, ids[0], (co_await redis_now(client.handle())) + 1000);
    const auto fresh = co_await client.handle().command("ZSCORE", presence::deadlines_key, tenant + "|" + ids[0]);
    require(co_await presence::seed_from_database(context, inspection.generation_), "repeated seed remains valid");
    const auto score_after = co_await client.handle().command("ZSCORE", presence::deadlines_key, tenant + "|" + ids[0]);
    require(score_before.string() != fresh.string() && fresh.string() == score_after.string(),
        "startup seed never replaces a newer committed heartbeat deadline");
    ruvia::db_schema cleanup({.driver_ = ruvia::db_driver::postgresql});
    cleanup.drop_table("sys_node");
    for (const auto& migration : cleanup.compile("presence_cleanup")) {
        (void)co_await db.execute(migration.sql());
    }
}

}  // namespace

int main() {
    try {
        const auto redis_config = test_support::isolated_redis_config();
        const auto postgres_config = test_support::isolated_postgres_config();
        if (!redis_config || !postgres_config) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*postgres_config);
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto first = loops.loop(0);
        const auto second = loops.loop(1);
        ruvia::redis_client left(first, *redis_config);
        ruvia::redis_client right(second, *redis_config);
        ruvia::db_client database(first, temporary_database.config());
        loops.start();
        std::exception_ptr failure;
        try {
            first.start(left.connect()).get();
            second.start(right.connect()).get();
            first.start(database.connect()).get();
            first.start(reset(left)).get();
            const auto candidate = first.start(prepare_expiry(left)).get();
            auto a = first.start(expire(left, candidate));
            auto b = second.start(expire(right, candidate));
            require(static_cast<int>(a.get()) + static_cast<int>(b.get()) == 1, "expiry is atomically claimed across workers");
            first.start(verify_expired(left, candidate)).get();
            first.start(verify_renewal_and_faults(left)).get();
            first.start(verify_publication_replay(left)).get();
            first.start(verify_generation(left)).get();
            first.start(verify_database_seed(database, left)).get();
            first.start(reset(left)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            first.start(left.shutdown()).get();
            second.start(right.shutdown()).get();
            first.start(database.shutdown()).get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        left.close();
        right.close();
        database.close();
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        std::cout << "Node presence: Redis atomic expiry, renewal, durable failure recovery, generations and PostgreSQL ORM seed passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
