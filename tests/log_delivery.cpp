#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/redis/redis_client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/features/log_ingest/ingest.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/logging/logger.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

namespace ingest = service::log_ingest;
namespace queue = ingest::queue;
namespace live = service::live_resource;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("log delivery: ") + message);
    }
}

struct delivery_context final {
    ruvia::db_client& database_;
    ruvia::redis_client& redis_;
    std::vector<std::string> own_claims_{};
    ruvia::db_handle db() const {
        return database_.handle();
    }
    ruvia::redis_handle redis() const {
        return redis_.handle();
    }
    ruvia::stop_token get_stop_token() const {
        return {};
    }
    // The claim loops this worker woke for work it committed itself.
    void record_own_claims(std::span<const std::string_view> types) {
        own_claims_.insert(own_claims_.end(), types.begin(), types.end());
    }
};

constexpr std::string_view tenant_id{"16e29f7c-46c7-48b6-bf6a-880ee2b998af"};
constexpr std::string_view node_id{"920dc14b-cc26-49e4-9937-93a7637bef79"};
constexpr std::string_view website_id{"b2d6e77d-4a0c-4796-93d5-755d3c6d3837"};
constexpr std::string_view agent_id{"0123456789abcdef0123456789abcdef"};
constexpr std::string_view consumer{"log-delivery-qa"};

flexedge::node::v2::LogDelivery delivery(std::string_view access_id, std::string_view log_id) {
    flexedge::node::v2::LogDelivery value;
    value.set_node_id(node_id);
    const auto occurred = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto* access = value.add_events()->mutable_access_log();
    access->set_id(access_id);
    access->set_website_id(website_id);
    access->set_occurred_unix_ms(occurred);
    access->set_client_ip("192.0.2.1");
    access->set_protocol("HTTP/2");
    access->set_method("GET");
    access->set_host("qa.example.com");
    access->set_target("/health");
    access->set_status_code(200);
    auto* log = value.add_events()->mutable_node_log();
    log->set_id(log_id);
    log->set_occurred_unix_ms(occurred);
    log->set_level("info");
    log->set_category("qa");
    log->set_message("transactional log delivery");
    return value;
}

ruvia::task<void> clean_redis(ruvia::redis_handle redis) {
    (void)co_await redis.command("DEL", queue::stream_key, queue::retained_bytes_key,
        queue::retry_schedule_key, queue::retry_attempts_key, queue::failed_stream_key,
        queue::failed_bytes_key, queue::statistics_key, ingest::notifications::stream_key);
}

ruvia::task<void> create_fixture(delivery_context& context) {
    auto db = context.db();
    (void)co_await db.execute("CREATE TEMP TABLE sys_node (id uuid, tenant_id uuid, cluster_id uuid, agent_id text, registration_status text, deleted_at timestamptz)");
    (void)co_await db.execute("CREATE TEMP TABLE sys_website (id uuid, tenant_id uuid, cluster_id uuid, deleted_at timestamptz)");
    (void)co_await db.execute("CREATE TEMP TABLE sys_website_access_log (tenant_id uuid, occurred_at timestamptz, id uuid, node_id uuid, website_id uuid, client_ip inet, protocol text, method text, host text, target text, status_code int, request_bytes bigint, response_bytes bigint, duration_ms bigint, user_agent text, referer text, request_headers text, request_body text, request_body_truncated bool, tls_fingerprint text, response_headers text, query_string text, cookies text, created_at timestamptz, PRIMARY KEY(occurred_at,id))");
    (void)co_await db.execute("CREATE TEMP TABLE sys_node_log (tenant_id uuid, occurred_at timestamptz, id uuid, node_id uuid, level text, category text, message text, created_at timestamptz, PRIMARY KEY(occurred_at,id))");
    (void)co_await db.execute("CREATE TEMP TABLE sys_notification_outbox (id varchar(64) PRIMARY KEY, kind varchar(24) NOT NULL, payload text NOT NULL, created_at timestamptz NOT NULL DEFAULT now(), available_at timestamptz NOT NULL DEFAULT now(), attempts integer NOT NULL DEFAULT 0)");
    (void)co_await db.execute("INSERT INTO sys_node VALUES($1,$2,'a2d6e77d-4a0c-4796-93d5-755d3c6d3837',$3,'registered',NULL)", node_id, tenant_id, agent_id);
    (void)co_await db.execute("INSERT INTO sys_website VALUES($1,$2,'a2d6e77d-4a0c-4796-93d5-755d3c6d3837',NULL)", website_id, tenant_id);
}

ruvia::task<void> check_counts(delivery_context& context, int access, int node, int outbox) {
    const auto rows = co_await context.db().query("SELECT (SELECT count(*) FROM sys_website_access_log),(SELECT count(*) FROM sys_node_log),(SELECT count(*) FROM sys_notification_outbox)");
    require(rows[0][0].as<int>() == access && rows[0][1].as<int>() == node && rows[0][2].as<int>() == outbox,
        "log rows and notification outbox share the transaction and retain idempotent counts");
}

ruvia::task<queue::entry> append(delivery_context& context, std::string_view payload) {
    require(co_await queue::push(context.redis(), payload), "the bounded Redis queue accepts the delivery");
    auto claimed = co_await queue::read(context.redis(), consumer);
    require(claimed.size() == 1, "a consumer claims its own delivery");
    co_return std::move(claimed.front());
}

ruvia::task<std::size_t> persist(delivery_context& context, const queue::entry& entry) {
    std::vector<queue::entry> entries{entry};
    co_return co_await ingest::process(context, consumer, std::move(entries));
}

ruvia::task<queue::entry> retry(delivery_context& context, const queue::entry& entry) {
    (void)co_await context.redis().command("ZADD", queue::retry_schedule_key, "0", entry.id_);
    auto claimed = co_await queue::claim_retries(context.redis(), consumer);
    require(claimed.size() == 1 && claimed.front().id_ == entry.id_, "a persisted retry deadline is recoverable");
    co_return std::move(claimed.front());
}

ruvia::task<void> deliver_notifications(delivery_context& context) {
    const auto events = co_await context.db().query("SELECT id FROM sys_notification_outbox");
    require(co_await live::outbox::deliver_batch<ingest::log_relay>(context), "committed outbox events are claimed and delivered");
    for (const auto& event : events) {
        const auto key = "flexedge:notification-published:v1:" + std::string(event[0].value().value_or(""));
        (void)co_await context.redis().command("DEL", key);
    }
}

ruvia::task<void> verify(delivery_context& context) {
    co_await clean_redis(context.redis());
    co_await queue::initialize(context.redis());
    co_await create_fixture(context);
    auto db = context.db();
    (void)co_await db.execute("CREATE FUNCTION pg_temp.reject_outbox() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'temporary QA database failure' USING ERRCODE='40001'; END $$");
    (void)co_await db.execute("CREATE TRIGGER reject_outbox BEFORE INSERT ON sys_notification_outbox FOR EACH ROW EXECUTE FUNCTION pg_temp.reject_outbox()");
    const auto value = delivery("b71e4ffc-56df-4e47-b947-b9a60e40996d", "2ca925d1-17cd-4b78-8d92-85752fe4f573");
    const auto payload = ingest::serialize_envelope(tenant_id, agent_id, value);
    auto entry = co_await append(context, payload);
    for (int attempt = 0; attempt < 6; ++attempt) {
        require(co_await persist(context, entry) == 0, "a database serialization failure is not acknowledged");
        co_await check_counts(context, 0, 0, 0);
        const auto statistics = co_await queue::inspect_statistics(context.redis());
        require(statistics.retained_entries_ == 1 && statistics.pending_entries_ == 1 && statistics.failed_entries_ == 0 &&
                    statistics.retries_total_ == attempt + 1,
            "temporary PostgreSQL failures remain in Redis beyond the former four-attempt limit");
        entry = co_await retry(context, entry);
    }
    (void)co_await db.execute("DROP TRIGGER reject_outbox ON sys_notification_outbox");
    require(co_await persist(context, entry) == 1, "the retained delivery succeeds when PostgreSQL recovers");
    co_await check_counts(context, 1, 1, 2);
    require(std::find(context.own_claims_.begin(), context.own_claims_.end(), "outbox") != context.own_claims_.end(),
        "the ingesting worker wakes only its own outbox loop for the notifications it committed");
    const auto duplicate = co_await append(context, payload);
    require(co_await persist(context, duplicate) == 1, "repeated node payloads complete idempotently");
    co_await check_counts(context, 1, 1, 2);

    (void)co_await context.redis().command("SET", ingest::notifications::stream_key, "wrong-type-fixture");
    require(co_await live::outbox::deliver_batch<ingest::log_relay>(context), "failed Redis publication leaves durable outbox work");
    co_await check_counts(context, 1, 1, 2);
    const auto deferred = co_await db.query("SELECT min(attempts),bool_and(available_at > now()) FROM sys_notification_outbox");
    require(deferred[0][0].as<int>() == 1 && deferred[0][1].as<bool>() == true, "outbox failure persists bounded retry scheduling");
    (void)co_await context.redis().command("DEL", ingest::notifications::stream_key);
    (void)co_await db.execute("UPDATE sys_notification_outbox SET available_at=now()");
    co_await deliver_notifications(context);
    co_await check_counts(context, 1, 1, 0);
    require((co_await ingest::notifications::read(context.redis(), "0-0")).notifications_.size() == 2,
        "recovered publication emits the committed access and node notifications");

    (void)co_await db.execute("CREATE FUNCTION pg_temp.reject_log() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'permanent QA log constraint' USING ERRCODE='23514'; END $$");
    (void)co_await db.execute("CREATE TRIGGER reject_log BEFORE INSERT ON sys_node_log FOR EACH ROW EXECUTE FUNCTION pg_temp.reject_log()");
    const auto rejected_value = delivery("a71e4ffc-56df-4e47-b947-b9a60e40996d", "3ca925d1-17cd-4b78-8d92-85752fe4f573");
    const auto rejected_payload = ingest::serialize_envelope(tenant_id, agent_id, rejected_value);
    const auto rejected = co_await append(context, rejected_payload);
    require(co_await persist(context, rejected) == 0, "permanent PostgreSQL data failure is archived for replay");
    co_await check_counts(context, 1, 1, 0);
    const auto failed = co_await queue::read_failed(context.redis());
    require(failed.size() == 1 && failed[0].source_id_ == rejected.id_ && failed[0].payload_ == rejected_payload &&
                failed[0].reason_ == "database_invalid_log_data:23514",
        "the failed delivery retains a precise non-sensitive error code");
    (void)co_await db.execute("DROP TRIGGER reject_log ON sys_node_log");
    require((co_await queue::replay_failed(context.redis(), failed[0].id_)).has_value(), "archived data can be explicitly replayed");
    auto replayed = co_await queue::read(context.redis(), consumer);
    require(replayed.size() == 1 && co_await persist(context, replayed[0]) == 1, "repaired data uses the normal persistence and acknowledgement path");
    co_await check_counts(context, 2, 2, 2);
    co_await deliver_notifications(context);
    co_await check_counts(context, 2, 2, 0);
    co_await clean_redis(context.redis());
}

}  // namespace

int main() {
    try {
        const auto postgres_config = test_support::isolated_postgres_config();
        const auto redis_config = test_support::isolated_redis_config();
        if (!postgres_config || !redis_config) {
            return 77;
        }
        const test_support::postgres_test_database temporary_database(*postgres_config);
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto loop = loops.loop(0);
        ruvia::db_client database(loop, temporary_database.config());
        ruvia::redis_client redis(loop, *redis_config);
        delivery_context context{database, redis};
        loops.start();
        std::exception_ptr failure;
        try {
            loop.start(database.connect()).get();
            loop.start(redis.connect()).get();
            loop.start(verify(context)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            loop.start(database.shutdown()).get();
            loop.start(redis.shutdown()).get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        database.close();
        redis.close();
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Log delivery: PostgreSQL rollback, Redis retry/replay, transactional outbox, idempotence passed\n";
    return 0;
}
