#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/redis/redis_client.h>

#include <algorithm>
#include <array>
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

#include "service/features/live_resource/notification_reader.h"
#include "service/features/log_ingest/notifications.h"
#include "service/features/log_ingest/worker.h"
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

std::int64_t now_milliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void set_occurred(flexedge::node::v2::LogDelivery& value, std::uint64_t occurred) {
    for (auto& event : *value.mutable_events()) {
        if (event.has_access_log()) {
            event.mutable_access_log()->set_occurred_unix_ms(occurred);
        } else {
            event.mutable_node_log()->set_occurred_unix_ms(occurred);
        }
    }
}

ingest::v2::QueuedLogDelivery envelope(const flexedge::node::v2::LogDelivery& value) {
    return *ingest::parse_envelope(ingest::serialize_envelope(tenant_id, agent_id, value));
}

ruvia::task<void> clean_redis(ruvia::redis_handle redis) {
    (void)co_await redis.command("DEL", queue::stream_key, queue::retained_bytes_key,
        queue::retry_schedule_key, queue::retry_attempts_key, queue::failed_stream_key,
        queue::failed_bytes_key, queue::statistics_key, ingest::notifications::stream_key);
}

ruvia::task<void> create_fixture(delivery_context& context) {
    auto db = context.db();
    (void)co_await db.execute("CREATE TABLE sys_node (id uuid, tenant_id uuid, cluster_id uuid, agent_id text, registration_status text, deleted_at timestamptz)");
    (void)co_await db.execute("CREATE TABLE sys_website (id uuid, tenant_id uuid, cluster_id uuid, deleted_at timestamptz)");
    (void)co_await db.execute("CREATE TABLE sys_website_access_log (tenant_id uuid, occurred_at timestamptz, id uuid, node_id uuid, website_id uuid, client_ip varchar(64), protocol text, method text, host text, target text, status_code int, request_bytes bigint, response_bytes bigint, duration_ms bigint, user_agent text, referer text, request_headers text, request_body text, request_body_truncated bool, tls_fingerprint text, response_headers text, query_string text, cookies text, cache_status varchar(16) NOT NULL DEFAULT '', cache_reason varchar(64) NOT NULL DEFAULT '', created_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(tenant_id,occurred_at,id))");
    (void)co_await db.execute("CREATE TABLE sys_node_log (tenant_id uuid, occurred_at timestamptz, id uuid, node_id uuid, level text, category text, message text, created_at timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(tenant_id,occurred_at,id))");
    (void)co_await db.execute("CREATE TABLE sys_notification_outbox (id varchar(64) PRIMARY KEY, kind varchar(24) NOT NULL, payload text NOT NULL, created_at timestamptz NOT NULL DEFAULT now(), available_at timestamptz NOT NULL DEFAULT now(), attempts integer NOT NULL DEFAULT 0)");
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
    co_return co_await ingest::persist_deliveries(context, consumer, std::move(entries));
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

ruvia::task<void> verify_batch_ownership(delivery_context& context) {
    auto db = context.db();
    (void)co_await db.execute("TRUNCATE sys_website_access_log,sys_node_log,sys_notification_outbox");
    constexpr std::string_view second_website{"23940d20-e632-492d-a305-45ba71bca319"};
    constexpr std::string_view foreign_cluster_website{"c738b4e3-d397-4d39-b141-3a98a11c2a1e"};
    constexpr std::string_view foreign_tenant_website{"891b194d-76a2-4a9e-ac3f-729605c10d9e"};
    constexpr std::string_view deleted_website{"bb9c5f55-1192-4ec7-960e-9b2745168fa0"};
    (void)co_await db.execute("INSERT INTO sys_website VALUES($1,$2,'a2d6e77d-4a0c-4796-93d5-755d3c6d3837',NULL),($3,$2,'c0d32d67-cc30-43c5-b931-a183d60b830f',NULL),($4,'db65ddad-849c-47ef-b1ae-341ea9129c49','a2d6e77d-4a0c-4796-93d5-755d3c6d3837',NULL),($5,$2,'a2d6e77d-4a0c-4796-93d5-755d3c6d3837',NOW())",
        second_website, tenant_id, foreign_cluster_website, foreign_tenant_website, deleted_website);
    auto batch = delivery("f32ac419-55af-4130-9168-02fe4d53f39c", "c91291b8-3ddf-48f1-8e0c-9e16f6ad0539");
    auto* access = batch.mutable_events(0)->mutable_access_log();
    access->set_request_bytes(1536);
    access->set_response_bytes(1099511627776ull);
    access->set_duration_ms(86400000);
    access->set_user_agent("agent ' $1");
    access->set_referer("https://example.com/path?q='value'");
    access->set_request_headers("x-quoted: '$2'\r\n");
    access->set_request_body("line 1\nline 2 ' \\");
    access->set_request_body_truncated(true);
    access->set_tls_fingerprint("tls-fingerprint");
    access->set_response_headers("content-type: text/plain");
    access->set_query_string("q='quoted'&id=$3");
    access->set_cookies("session=log-fixture");
    access->set_cache_status("hit_disk");
    access->set_cache_reason("origin_not_modified");
    access->set_cache_revalidated(true);
    const auto first_access = batch.events(0);
    const auto first_node = batch.events(1);
    *batch.add_events() = first_access;
    batch.mutable_events(2)->mutable_access_log()->set_id("e5643be6-049e-4f05-b0df-ea9275144d1a");
    batch.mutable_events(2)->mutable_access_log()->set_website_id(second_website);
    *batch.add_events() = first_access;
    batch.mutable_events(3)->mutable_access_log()->set_id("adf76493-b06e-4035-aef7-57b75f8d86dc");
    *batch.add_events() = first_node;
    batch.mutable_events(4)->mutable_node_log()->set_id("37753e1e-3b25-42df-9292-4cb0ad68dcc9");
    *batch.add_events() = first_access;
    const auto payload = ingest::serialize_envelope(tenant_id, agent_id, batch);
    const auto entry = co_await append(context, payload);
    require(co_await persist(context, entry) == 1, "one mixed batch persists every distinct row, ignoring duplicates within the batch");
    co_await check_counts(context, 3, 2, 3);
    const auto stored = co_await db.query("SELECT request_bytes,response_bytes,duration_ms,user_agent,referer,request_headers,request_body,request_body_truncated,tls_fingerprint,response_headers,query_string,cookies,round(extract(epoch FROM occurred_at)*1000)::bigint,created_at IS NOT NULL FROM sys_website_access_log WHERE id=$1", first_access.access_log().id());
    const auto& expected = first_access.access_log();
    const auto cache_status = co_await db.query("SELECT cache_status,cache_reason FROM sys_website_access_log WHERE id=$1", first_access.access_log().id());
    require(cache_status.front()[0].value() == "revalidated" && cache_status.front()[1].value() == "origin_not_modified", "cache tier is persisted independently of captured response headers");
    require(stored.size() == 1 && stored[0][0].as<std::int64_t>() == 1536 &&
                stored[0][1].as<std::int64_t>() == 1099511627776ll && stored[0][2].as<int>() == 86400000 &&
                stored[0][3].value() == expected.user_agent() && stored[0][4].value() == expected.referer() &&
                stored[0][5].value() == expected.request_headers() && stored[0][6].value() == expected.request_body() &&
                stored[0][7].as<bool>() == true && stored[0][8].value() == expected.tls_fingerprint() &&
                stored[0][9].value() == expected.response_headers() && stored[0][10].value() == expected.query_string() &&
                stored[0][11].value() == expected.cookies() &&
                stored[0][12].as<std::int64_t>() == static_cast<std::int64_t>(expected.occurred_unix_ms()) &&
                stored[0][13].as<bool>() == true,
        "entity bulk writes retain every optional field, bound text, integer limit and UTC millisecond timestamp");
    co_await deliver_notifications(context);
    const auto duplicate = co_await append(context, payload);
    require(co_await persist(context, duplicate) == 1, "a complete duplicate batch is acknowledged");
    co_await check_counts(context, 3, 2, 0);

    auto partial = batch;
    partial.mutable_events(3)->mutable_access_log()->set_id("732c41ea-b088-47e3-9000-25cc82933a6e");
    const auto partial_entry = co_await append(context, ingest::serialize_envelope(tenant_id, agent_id, partial));
    require(co_await persist(context, partial_entry) == 1, "a partially duplicate mixed batch writes only its new row");
    co_await check_counts(context, 4, 2, 1);
    co_await deliver_notifications(context);

    for (const auto website : {foreign_cluster_website, foreign_tenant_website, deleted_website}) {
        auto rejected = batch;
        rejected.mutable_events(0)->mutable_access_log()->set_id("e053e8a9-f0a2-43de-834c-260f7c86933e");
        rejected.mutable_events(1)->mutable_node_log()->set_id("1944fedb-974e-4ca3-8040-e753a2a34da5");
        rejected.mutable_events(2)->mutable_access_log()->set_website_id(website);
        const auto rejected_entry = co_await append(context, ingest::serialize_envelope(tenant_id, agent_id, rejected));
        require(co_await persist(context, rejected_entry) == 0, "the entire batch is rejected when any website is outside the active node cluster");
        co_await check_counts(context, 4, 2, 0);
    }
    const auto revoked = co_await append(context, ingest::serialize_envelope(tenant_id,
                                                      "fedcba9876543210fedcba9876543210", batch));
    require(co_await persist(context, revoked) == 0, "node ownership is verified against the accepted agent identity");
    co_await check_counts(context, 4, 2, 0);
}

ruvia::task<void> verify_corrected_timestamp_identity(delivery_context& context) {
    auto db = context.db();
    (void)co_await db.execute("TRUNCATE sys_website_access_log,sys_node_log,sys_notification_outbox");
    co_await clean_redis(context.redis());
    co_await queue::initialize(context.redis());
    const auto received = now_milliseconds();
    const auto future = static_cast<std::uint64_t>(received + 60 * 60 * 1000);
    auto batch = delivery("CE798C58-BC02-4654-A764-35674D6873AF", "ce798c58-bc02-4654-a764-35674d6873af");
    set_occurred(batch, future);
    const auto first_access = batch.events(0);
    const auto first_node = batch.events(1);
    *batch.add_events() = first_access;
    batch.mutable_events(2)->mutable_access_log()->set_id("ce798c58-bc02-4654-a764-35674d6873af");
    batch.mutable_events(2)->mutable_access_log()->set_occurred_unix_ms(received - 1000);
    *batch.add_events() = first_node;
    batch.mutable_events(3)->mutable_node_log()->set_id("CE798C58-BC02-4654-A764-35674D6873AF");
    batch.mutable_events(3)->mutable_node_log()->set_occurred_unix_ms(received - 2000);
    const auto queued = envelope(batch);
    co_await ingest::insert_envelope(context, queued, received);
    co_await check_counts(context, 1, 1, 2);
    const auto times = co_await db.query("SELECT round(extract(epoch FROM occurred_at)*1000)::bigint FROM sys_website_access_log UNION ALL SELECT round(extract(epoch FROM occurred_at)*1000)::bigint FROM sys_node_log");
    require(times.size() == 2 && times[0][0].as<std::int64_t>() == received && times[1][0].as<std::int64_t>() == received,
        "within-batch identity ignores timestamp and UUID case; kinds remain independent and first corrected times win");
    co_await deliver_notifications(context);
    // The exact wire payload is retried after a lost ACK, including after its
    // formerly future timestamp enters the normal acceptance window.
    for (const auto next_received : {received + 1, received + 60000, received + 56 * 60 * 1000,
             received + 61 * 60 * 1000}) {
        co_await ingest::insert_envelope(context, queued, next_received);
        co_await check_counts(context, 1, 1, 0);
    }
    const auto stable = co_await db.query("SELECT round(extract(epoch FROM occurred_at)*1000)::bigint FROM sys_website_access_log");
    require(stable[0][0].as<std::int64_t>() == received, "redelivery never rewrites the first committed occurred_at");

    auto partial = batch;
    *partial.add_events() = first_access;
    partial.mutable_events(4)->mutable_access_log()->set_id("fcaa4270-976c-4453-99b6-ab67e7d16224");
    *partial.add_events() = first_node;
    partial.mutable_events(5)->mutable_node_log()->set_id("bca092b8-5288-42f4-a8ea-5b84e3f9688a");
    co_await ingest::insert_envelope(context, envelope(partial), received + 1000);
    co_await check_counts(context, 2, 2, 2);
    co_await deliver_notifications(context);

    auto rejected = delivery("4a3edb05-c194-4699-9a06-983ed078b63c", "71c488df-93ad-42df-9779-4375c8daaecb");
    set_occurred(rejected, future);
    (void)co_await db.execute("CREATE TRIGGER reject_corrected_log BEFORE INSERT ON sys_node_log FOR EACH ROW EXECUTE FUNCTION reject_log()");
    const auto rejected_entry = co_await append(context, ingest::serialize_envelope(tenant_id, agent_id, rejected));
    require(co_await persist(context, rejected_entry) == 0, "a mixed corrected batch rolls back before being archived");
    co_await check_counts(context, 2, 2, 0);
    const auto archived = co_await queue::read_failed(context.redis());
    require(archived.size() == 1, "the rolled-back corrected batch remains replayable");
    (void)co_await db.execute("DROP TRIGGER reject_corrected_log ON sys_node_log");
    co_await ingest::insert_envelope(context, envelope(rejected), received + 2000);
    co_await check_counts(context, 3, 3, 2);
    co_await deliver_notifications(context);
    require((co_await queue::replay_failed(context.redis(), archived[0].id_)).has_value(), "dead-letter replay obtains a new Redis receipt");
    const auto replayed = co_await queue::read(context.redis(), consumer);
    require(replayed.size() == 1 && co_await persist(context, replayed[0]) == 1,
        "replay after another delivery committed is acknowledged as the same events");
    co_await check_counts(context, 3, 3, 0);
}

inline constexpr std::int64_t ingest_barrier_key{491724383};

ruvia::task<void> prepare_concurrent_ingest(delivery_context& context) {
    auto db = context.db();
    (void)co_await db.execute("TRUNCATE sys_website_access_log,sys_node_log,sys_notification_outbox");
    (void)co_await db.execute(
        "CREATE FUNCTION hold_log_ingest() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN "
        "IF current_setting('transaction_isolation') <> 'read committed' THEN RAISE EXCEPTION 'log ingest must use READ COMMITTED'; END IF; "
        "PERFORM pg_advisory_xact_lock(491724383); RETURN NEW; END $$");
    (void)co_await db.execute("CREATE TRIGGER hold_log_ingest BEFORE INSERT ON sys_website_access_log FOR EACH ROW EXECUTE FUNCTION hold_log_ingest()");
}

ruvia::task<bool> wait_for_ingest_locks(ruvia::db_client& observer, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waiting = co_await observer.handle().query(
            "SELECT count(*) FROM pg_stat_activity WHERE datname=current_database() AND wait_event_type='Lock'");
        if (waiting[0][0].as<int>().value_or(0) >= expected) {
            co_return true;
        }
        (void)co_await ruvia::sleep_for(observer.worker(), std::chrono::milliseconds(10));
    }
    co_return false;
}

ruvia::task<void> persist_owned_envelope(delivery_context& context, ingest::v2::QueuedLogDelivery value,
    std::int64_t received) {
    co_await ingest::insert_envelope(context, value, received);
}

ruvia::task<void> configure_ingest_session(ruvia::db_client& database) {
    (void)co_await database.handle().execute("SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL REPEATABLE READ");
}

ruvia::task<void> change_ingest_barrier(ruvia::db_client& observer, bool acquire) {
    (void)co_await observer.handle().query(acquire ? "SELECT pg_advisory_lock($1)" : "SELECT pg_advisory_unlock($1)", ingest_barrier_key);
}

ruvia::task<void> remove_ingest_barrier(delivery_context& context) {
    (void)co_await context.db().execute("DROP TRIGGER hold_log_ingest ON sys_website_access_log");
    (void)co_await context.db().execute("DROP FUNCTION hold_log_ingest()");
}

// Each real Worker owns its own DB/Redis clients and an independent envelope.
// A separate session holds the first writer inside INSERT. Observed PG lock
// waits ensure the second has started before either writer can commit.
void verify_concurrent_ingest(const ruvia::event_loop& first, delivery_context& context,
    const ruvia::event_loop& second, delivery_context& peer, ruvia::db_client& observer) {
    first.start(prepare_concurrent_ingest(context)).get();
    first.start(configure_ingest_session(context.database_)).get();
    second.start(configure_ingest_session(peer.database_)).get();
    first.start(change_ingest_barrier(observer, true)).get();
    const auto received = now_milliseconds();
    auto batch = delivery("a8f276c6-f80c-424d-95e1-f5e761103a21", "efc320fd-2c48-4fa3-8a82-0ab6d5f42c64");
    set_occurred(batch, static_cast<std::uint64_t>(received + 3600000));
    auto writing = first.start(persist_owned_envelope(context, envelope(batch), received));
    std::exception_ptr failure;
    try {
        require(first.start(wait_for_ingest_locks(observer, 1)).get(), "the first writer reached its database barrier");
    } catch (...) {
        failure = std::current_exception();
    }
    auto concurrent = second.start(persist_owned_envelope(peer, envelope(batch), received + 60000));
    try {
        require(first.start(wait_for_ingest_locks(observer, 2)).get(), "both independent Workers reached a PostgreSQL lock wait");
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
    }
    try {
        first.start(change_ingest_barrier(observer, false)).get();
    } catch (...) {
        if (!failure) {
            failure = std::current_exception();
        }
        // Closing the observing session also releases its advisory barrier.
        first.start(observer.shutdown()).get();
    }
    for (auto* operation : {&writing, &concurrent}) {
        try {
            operation->get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    first.start(remove_ingest_barrier(context)).get();
    if (failure) {
        std::rethrow_exception(failure);
    }
    first.start(check_counts(context, 1, 1, 2)).get();
    first.start(deliver_notifications(context)).get();
    first.start(check_counts(context, 1, 1, 0)).get();
}

ruvia::task<void> verify(delivery_context& context) {
    co_await clean_redis(context.redis());
    co_await queue::initialize(context.redis());
    co_await create_fixture(context);
    auto db = context.db();
    (void)co_await db.execute("CREATE FUNCTION reject_outbox() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'temporary QA database failure' USING ERRCODE='40001'; END $$");
    (void)co_await db.execute("CREATE TRIGGER reject_outbox BEFORE INSERT ON sys_notification_outbox FOR EACH ROW EXECUTE FUNCTION reject_outbox()");
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
    const std::array notification_keys{ingest::notifications::stream_key};
    const std::array<live::notification_cursor, 1> notification_cursors{};
    const auto notification_batch = co_await live::read_notification_batch(
        context.redis(), notification_keys, notification_cursors, false);
    const auto notifications = notification_batch.array()[1].array()[0].array()[1].array();
    require(notifications.size() == 2 && ingest::notifications::stream::parse(notifications[0]) &&
                ingest::notifications::stream::parse(notifications[1]),
        "recovered publication emits the committed access and node notifications");

    (void)co_await db.execute("CREATE FUNCTION reject_log() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'permanent QA log constraint' USING ERRCODE='23514'; END $$");
    (void)co_await db.execute("CREATE TRIGGER reject_log BEFORE INSERT ON sys_node_log FOR EACH ROW EXECUTE FUNCTION reject_log()");
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
    co_await verify_batch_ownership(context);
    co_await verify_corrected_timestamp_identity(context);
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
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        const auto loop = loops.loop(0);
        const auto peer_loop = loops.loop(1);
        ruvia::db_client database(loop, temporary_database.config());
        ruvia::redis_client redis(loop, *redis_config);
        ruvia::db_client peer_database(peer_loop, temporary_database.config());
        ruvia::redis_client peer_redis(peer_loop, *redis_config);
        ruvia::db_client observer(loop, temporary_database.config());
        delivery_context context{database, redis};
        delivery_context peer{peer_database, peer_redis};
        loops.start();
        std::exception_ptr failure;
        try {
            loop.start(database.connect()).get();
            loop.start(redis.connect()).get();
            peer_loop.start(peer_database.connect()).get();
            peer_loop.start(peer_redis.connect()).get();
            loop.start(observer.connect()).get();
            loop.start(verify(context)).get();
            verify_concurrent_ingest(loop, context, peer_loop, peer, observer);
        } catch (...) {
            failure = std::current_exception();
        }
        const auto shutdown = [&](const ruvia::event_loop& owner, auto& client) {
            try {
                owner.start(client.shutdown()).get();
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        };
        shutdown(loop, database);
        shutdown(loop, redis);
        shutdown(peer_loop, peer_database);
        shutdown(peer_loop, peer_redis);
        shutdown(loop, observer);
        database.close();
        redis.close();
        peer_database.close();
        peer_redis.close();
        observer.close();
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
