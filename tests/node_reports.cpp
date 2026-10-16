#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/db/db_client.h>
#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/redis/redis_client.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "node/proto/schema_version.h"
#include "service/config/schema.h"
#include "service/features/node_channel/report_persistence.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

namespace channel = service::node_channel;
namespace queue = channel::report_queue;

constexpr std::string_view tenant{"00000000-0000-4000-8000-0000000004a1"};
constexpr std::string_view node{"00000000-0000-4000-8000-0000000004a2"};
constexpr std::string_view cluster{"00000000-0000-4000-8000-0000000004a3"};
constexpr std::string_view provider{"00000000-0000-4000-8000-0000000004a4"};
constexpr std::string_view zone{"00000000-0000-4000-8000-0000000004a5"};
constexpr std::string_view release{"00000000-0000-4000-8000-0000000004a6"};
constexpr std::string_view agent{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
const std::string hash(64, 'b');
std::string digest;
const std::string identity_key = "flexedge:node-channel:v1:agent:" + std::string(agent);
constexpr std::string_view consumer{"node-reports-owner"};
constexpr std::string_view other_consumer{"node-reports-new-owner"};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("node reports: " + std::string(message));
    }
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct report_context final {
    ruvia::db_client& database_;
    ruvia::redis_client& redis_;
    ruvia::worker_handle worker_;
    service::background::work_available::claim_signals claims_;
    ruvia::stop_source stop_;
    ruvia::db_handle db() const {
        return database_.handle();
    }
    ruvia::redis_handle redis() const {
        return redis_.handle();
    }
    const ruvia::worker_handle& worker() const {
        return worker_;
    }
    ruvia::stop_token get_stop_token() const {
        return stop_.token();
    }
    std::pmr::memory_resource* pool() const {
        return std::pmr::get_default_resource();
    }
    template <typename state_type>
    state_type& worker_state() {
        static_assert(std::is_same_v<state_type, service::background::work_available::claim_signals>);
        return claims_;
    }
};

channel::report heartbeat(std::int64_t received = now_ms()) {
    channel::report result{.principal_ = {.node_id_ = std::string(node), .cluster_id_ = std::string(cluster), .tenant_id_ = std::string(tenant), .agent_id_ = std::string(agent)},
        .verified_hash_ = hash,
        .received_unix_ms_ = received,
        .envelope_ = {},
        .elapsed_milliseconds_ = std::nullopt,
        .trace_ = std::nullopt};
    result.envelope_.set_request_id("heartbeat-report");
    auto& value = *result.envelope_.mutable_heartbeat();
    value.set_node_id(node);
    value.set_applied_node_spec_revision(1);
    value.set_active_release_id(release);
    value.set_active_manifest_digest(digest);
    value.set_agent_version("node-reports-qa");
    value.set_cpu_usage(0.25);
    value.set_memory_usage(0.5);
    value.set_health("healthy");
    return result;
}

channel::report failed_apply() {
    auto result = heartbeat();
    result.envelope_.clear_heartbeat();
    result.envelope_.set_request_id("apply-report");
    auto& value = *result.envelope_.mutable_apply_result();
    value.set_node_id(node);
    value.set_node_spec_revision(1);
    value.set_release_id(release);
    value.set_manifest_digest(digest);
    value.set_applied(false);
    value.set_failed_phase(flexedge::node::v2::APPLY_PHASE_VALIDATE);
    value.set_error_code("qa_validation_failed");
    value.set_error("isolated QA failure");
    value.set_retryable(true);
    result.elapsed_milliseconds_ = 123;
    result.trace_ = service::attempt_trace::stored_trace{"{\"qa\":true}", "{\"steps\":[\"immutable\"]}", 23};
    return result;
}

ruvia::task<std::int64_t> scalar(ruvia::db_handle db, std::string_view sql) {
    const auto rows = co_await db.query(sql);
    co_return rows[0][0].as<std::int64_t>().value_or(-1);
}

ruvia::task<void> fixture(report_context& context) {
    flexedge::node::v2::ClusterReleaseContent content;
    content.set_release_id(release);
    content.set_cluster_id(cluster);
    content.set_generation(1);
    content.set_schema_version(flexedge::node::cluster_release_schema_version);
    const auto manifest =
        flexedge::node::pack_artifact<flexedge::node::v2::ClusterReleaseManifest>(content);
    digest = manifest.digest_sha256();
    const auto manifest_envelope = service::utils::seal_secret(flexedge::node::serialize_artifact(manifest));
    auto db = context.db();
    (void)co_await db.execute("INSERT INTO sys_tenant(id,name,slug) VALUES($1,'Node reports QA','node-reports-qa')", tenant);
    (void)co_await db.execute("INSERT INTO sys_provider(id,tenant_id,kind,provider,name,account_id,status) VALUES($1,$2,'dns','cloudflare','QA','qa','verified')", provider, tenant);
    (void)co_await db.execute("INSERT INTO sys_dns_zone(id,tenant_id,provider_id,domain) VALUES($1,$2,$3,'reports.example.com')", zone, tenant, provider);
    (void)co_await db.execute("INSERT INTO sys_cluster(id,tenant_id,dns_zone_id,name,hostname_prefix,release_generation) VALUES($1,$2,$3,'Reports','edge',1)", cluster, tenant, zone);
    (void)co_await db.execute("INSERT INTO sys_cluster_release(id,tenant_id,cluster_id,generation,status,manifest_digest,manifest_envelope,schema_version) VALUES($1,$2,$3,1,'active',$4,$5,$6)", release, tenant, cluster, digest, manifest_envelope, flexedge::node::cluster_release_schema_version);
    (void)co_await db.execute("INSERT INTO sys_node(id,tenant_id,cluster_id,name,registration_status,registered_at,agent_id,node_secret_hash,node_secret_envelope,desired_release_id) VALUES($1,$2,$3,'Reports node','registered',NOW(),$4,$5,'qa-only',$6)", node, tenant, cluster, agent, hash, release);
    (void)co_await db.execute("UPDATE sys_cluster SET current_release_id=$1 WHERE id=$2", release, cluster);
    (void)co_await db.execute("INSERT INTO sys_node_release_target(tenant_id,release_id,node_id,status) VALUES($1,$2,$3,'pending')", tenant, release, node);
    (void)co_await context.redis().command("DEL", queue::stream_key, queue::bytes_key, queue::retry_key, queue::attempts_key, queue::positions_key,
        queue::cleanup_key, queue::cleanup_identity_key, queue::failed_key, queue::failed_bytes_key, channel::completion_stream_key,
        identity_key);
    (void)co_await context.redis().command("HSET", identity_key, "tenant_id", tenant, "node_id", node, "cluster_id", cluster,
        "secret_hash", hash, "status", "registered", "agent_id", agent);
    co_await queue::initialize(context.redis());
}

ruvia::task<queue::entry> append(report_context& context, const channel::report& report) {
    const auto id = co_await channel::enqueue(context.redis(), report);
    require(id.has_value(), "verified identity can enqueue within the Redis budget");
    const auto entry = co_await queue::claim(context.redis(), consumer);
    require(entry && entry->report_id_ == *id, "one immutable report is claimed by its consumer");
    co_return *entry;
}

ruvia::task<void> verify_authentication_projection(report_context& context) {
    auto report = heartbeat();
    report.envelope_.clear_heartbeat();
    report.envelope_.set_request_id("sanitized-authentication");
    report.envelope_.mutable_authenticate()->set_node_id(agent);
    report.envelope_.mutable_authenticate()->set_agent_version("node-reports-qa");
    const auto entry = co_await append(context, report);
    co_await channel::persist_claim(context, consumer, entry);
    require((co_await channel::completion(context.redis(), entry.report_id_)) == true,
        "sanitized authentication completes after its hashed DB credential was verified");
    const auto projected = co_await channel::identity(context.redis(), agent);
    require(projected && projected->status_ == "registered" && (co_await channel::desired(context.redis(), report.principal_)),
        "authentication completion is not published before its Redis desired state is readable");
    (void)co_await channel::clean_report_receipts(context);
}

ruvia::task<void> verify_queue(report_context& context) {
    auto report = heartbeat();
    const auto encoded = queue::encode(report);
    const auto decoded = queue::decode(encoded);
    require(decoded && decoded->envelope_.SerializeAsString() == report.envelope_.SerializeAsString() &&
                decoded->received_unix_ms_ == report.received_unix_ms_,
        "length-prefixed codec preserves binary protobuf and original arrival time");
    auto auth = report;
    auth.envelope_.clear_heartbeat();
    auth.envelope_.mutable_authenticate()->set_node_id(agent);
    auth.envelope_.mutable_authenticate()->set_secret("must-never-enter-redis");
    bool secret_rejected{};
    try {
        (void)queue::encode(auth);
    } catch (const std::invalid_argument&) {
        secret_rejected = true;
    }
    require(secret_rejected, "plaintext authentication secret is rejected before Redis I/O");
    report.verified_hash_.assign(64, 'd');
    require(!(co_await channel::enqueue(context.redis(), report)), "Lua rejects a stale credential hash atomically");
    report = heartbeat();
    report.principal_.tenant_id_ = "00000000-0000-4000-8000-0000000004ff";
    require(!(co_await channel::enqueue(context.redis(), report)), "Lua rejects identity tenant substitution");
    report = heartbeat();
    (void)co_await context.redis().command("SET", queue::bytes_key, std::to_string(queue::max_bytes));
    require(!(co_await channel::enqueue(context.redis(), report)), "byte budget applies backpressure before append");
    (void)co_await context.redis().command("DEL", queue::bytes_key);
    const auto entry = co_await append(context, report);
    require(!(co_await channel::completion(context.redis(), entry.report_id_)), "enqueue acceptance is not a committed completion");
    require(!(co_await queue::owns(context.redis(), other_consumer, entry)), "another consumer cannot acknowledge its peer's report");
    require(co_await queue::complete(context.redis(), other_consumer, entry, true, "") == -1,
        "completion Lua fences a stale owner");
    (void)co_await context.redis().command("SET", queue::failed_bytes_key, std::to_string(queue::max_failed_bytes));
    require(co_await queue::complete(context.redis(), consumer, entry, false, "qa_queue_only") == 0 &&
                !(co_await channel::completion(context.redis(), entry.report_id_)) && co_await queue::owns(context.redis(), consumer, entry),
        "a full failure archive retains its original input and emits no false completion");
    (void)co_await context.redis().command("DEL", queue::failed_bytes_key);
    require(co_await queue::complete(context.redis(), consumer, entry, false, "qa_queue_only") == 1,
        "permanent rejection archives the immutable input and publishes a false receipt");
    require((co_await channel::completion(context.redis(), entry.report_id_)) == false, "rejected completion is distinct from absent");
}

ruvia::task<void> verify_apply_transaction(report_context& context) {
    const auto report = failed_apply();
    const auto entry = co_await append(context, report);
    (void)co_await context.db().execute("CREATE FUNCTION reject_report_outbox() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'isolated retry' USING ERRCODE='40001'; END $$");
    (void)co_await context.db().execute("CREATE TRIGGER reject_report_outbox BEFORE INSERT ON sys_notification_outbox FOR EACH ROW EXECUTE FUNCTION reject_report_outbox()");
    for (int attempt = 0; attempt < 6; ++attempt) {
        co_await channel::persist_claim(context, consumer, entry);
        require(co_await scalar(context.db(), "SELECT count(*) FROM sys_node_report_receipt") == 0 &&
                    co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event") == 0 &&
                    co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event_trace") == 0,
            "database failure rolls back receipt, apply attempt, immutable trace and outbox together");
        require(!(co_await channel::completion(context.redis(), entry.report_id_)), "six temporary failures remain unacknowledged");
        const auto attempts = co_await context.redis().command("HGET", queue::attempts_key, entry.id_);
        require(queue::integer(attempts.string()) == attempt + 1, "retry attempts live in Redis beyond the old four-try limit");
    }
    (void)co_await context.db().execute("DROP TRIGGER reject_report_outbox ON sys_notification_outbox");
    require(co_await channel::persist_report(context, consumer, entry, report), "database recovery commits the retained immutable report");
    // A failed apply of an unapplied node spec records the publish and the
    // configure attempt of the node task; the trace belongs to the first.
    require(co_await scalar(context.db(), "SELECT count(*) FROM sys_node_report_receipt") == 1 &&
                co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event WHERE operation = 'publish'") == 1 &&
                co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event WHERE operation = 'configure'") == 1 &&
                co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event_trace") == 1,
        "one transaction stores receipt, attempts and trace");
    require(!(co_await channel::completion(context.redis(), entry.report_id_)), "DB commit alone does not fabricate a Redis completion");
    require(co_await channel::persist_report(context, consumer, entry, report), "crash after DB commit can safely replay before Redis finish");
    require(co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event") == 2,
        "same report ID cannot create a second failure attempt");
    (void)co_await context.redis().command("XCLAIM", queue::stream_key, queue::group, other_consumer, "0", entry.id_);
    require(!(co_await channel::persist_report(context, consumer, entry, report)),
        "the DB row lock is followed by a fresh ownership fence after takeover");
    require(co_await channel::persist_report(context, other_consumer, entry, report),
        "the replacement owner sees the existing committed receipt");
    const auto trace = co_await context.db().query("SELECT summary->>'qa',trace->'steps'->>0 FROM sys_sync_event_trace");
    require(trace[0][0].value() == "true" && trace[0][1].value() == "immutable", "trace bytes survive the queue without a live recorder reference");
    require(co_await queue::complete(context.redis(), other_consumer, entry, true, "", &report.principal_) == 1,
        "receipt, completion event, cleanup record and queue removal are atomic after commit");
    require((co_await channel::completion(context.redis(), entry.report_id_)) == true, "original connection sees DB committed completion");
    require(co_await channel::clean_report_receipts(context) == 1 &&
                co_await scalar(context.db(), "SELECT count(*) FROM sys_node_report_receipt") == 0,
        "safe cleanup removes completed ledger rows without retaining heartbeat history forever");
    require(!(co_await channel::persist_report(context, other_consumer, entry, report)),
        "a delayed old owner cannot reapply after the ledger row was cleaned");
    require(co_await scalar(context.db(), "SELECT count(*) FROM sys_sync_event") == 2,
        "cleanup preserves exactly the durable business attempts of the report");
}

ruvia::task<void> delayed_persistence(report_context& context, const queue::entry& entry, const channel::report& report,
    std::optional<bool>& completed) {
    completed = co_await channel::persist_report(context, consumer, entry, report);
}

ruvia::task<void> verify_delayed_owner(report_context& context, report_context& peer) {
    const auto report = heartbeat(now_ms() + 1);
    const auto entry = co_await append(context, report);
    require(co_await channel::persist_report(context, consumer, entry, report), "initial owner commits before the completion publication");
    auto blocker = co_await context.db().begin_transaction();
    (void)co_await channel::lock_report_node(blocker, tenant, node);
    std::optional<bool> delayed_result;
    ruvia::task_scope pending(context.worker());
    pending.spawn(delayed_persistence(peer, entry, report, delayed_result));
    (void)co_await ruvia::sleep_for(context.worker(), std::chrono::milliseconds(50), {});
    require(!delayed_result.has_value(), "a delayed duplicate waits behind the node row lock");
    require(co_await queue::complete(context.redis(), consumer, entry, true, "", &report.principal_) == 1,
        "the committed source can finish while its stale duplicate is waiting");
    co_await blocker.commit();
    co_await pending.join();
    require(delayed_result == false, "the delayed duplicate rechecks source existence after acquiring the row lock");
    require(co_await channel::clean_report_receipts(context) == 1, "ledger cleanup completes after the delayed owner is fenced");
    require(!(co_await channel::persist_report(peer, consumer, entry, report)),
        "the same delayed delivery also stays rejected after ledger deletion");
}

ruvia::task<void> verify_worker_loop(report_context& context) {
    ruvia::task_scope tasks(context.worker());
    tasks.spawn(channel::run_reports(context));
    const auto id = co_await channel::enqueue(context.redis(), heartbeat(now_ms() + 2));
    require(id.has_value(), "a new report is accepted while the replica claim loop is running");
    // The enqueuing worker wakes only its own claim loop.
    channel::wake_own_reports(context);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<bool> complete;
    while (!(complete = co_await channel::completion(context.redis(), *id)) && std::chrono::steady_clock::now() < deadline) {
        (void)co_await ruvia::sleep_for(context.worker(), std::chrono::milliseconds(10), {});
    }
    require(complete == true, "the actual nonblocking claim loop persists and completes the report after its wake hint");
    context.stop_.request_stop();
    const auto stopped = std::chrono::steady_clock::now();
    co_await tasks.join();
    require(std::chrono::steady_clock::now() - stopped < std::chrono::seconds(1), "canceling the idle claim loop releases its wait without a blocked Redis connection");
}

ruvia::task<void> verify_redis_reset_collision(report_context& context) {
    const auto report = heartbeat(now_ms() + 3);
    const auto old = co_await append(context, report);
    require(co_await channel::persist_report(context, consumer, old, report), "old queue event commits before a Redis reset");
    (void)co_await context.redis().command("DEL", queue::stream_key, queue::bytes_key, queue::retry_key, queue::attempts_key,
        queue::positions_key, queue::cleanup_key, queue::cleanup_identity_key);
    co_await queue::initialize(context.redis());
    const auto new_report_id = service::utils::random_token();
    const auto payload = queue::encode(report);
    (void)co_await context.redis().command("XADD", queue::stream_key, old.id_, "payload", payload, "report_id", new_report_id);
    (void)co_await context.redis().command("HSET", queue::positions_key, new_report_id, old.id_);
    (void)co_await context.redis().command("SET", queue::bytes_key, std::to_string(payload.size() + new_report_id.size() + 128));
    const auto replacement = co_await queue::claim(context.redis(), consumer);
    require(replacement && replacement->id_ == old.id_ && replacement->report_id_ != old.report_id_,
        "recreated stream can reuse its timestamp ID while its public report ID remains unique");
    require(!(co_await channel::persist_report(context, consumer, old, report)) &&
                co_await queue::complete(context.redis(), consumer, old, true, "", &report.principal_) == -1,
        "old owner cannot mutate or acknowledge a different report at the reused Redis stream ID");
    require(co_await channel::persist_report(context, consumer, *replacement, report), "replacement event does not collide with the old PostgreSQL receipt");
    require(co_await scalar(context.db(), "SELECT count(*) FROM sys_node_report_receipt") == 2,
        "two distinct immutable events keep independent commit receipts despite stream ID reuse");
    std::string cursor;
    require(co_await channel::clean_orphan_receipts(context, cursor) == 1 &&
                co_await scalar(context.db(), "SELECT count(*) FROM sys_node_report_receipt") == 1,
        "Redis reset orphan is reclaimed while the replacement's live source keeps its receipt");
    require(co_await queue::complete(context.redis(), consumer, *replacement, true, "", &report.principal_) == 1,
        "replacement report completes independently");
    require(!(co_await channel::completion(context.redis(), old.report_id_)) &&
                (co_await channel::completion(context.redis(), replacement->report_id_)) == true,
        "an old connection never mistakes the replacement completion for its own");
    (void)co_await channel::clean_report_receipts(context);
}

ruvia::task<void> verify_heartbeat(report_context& context) {
    const auto received = now_ms() - 120000;
    auto report = heartbeat(received);
    auto entry = co_await append(context, report);
    co_await channel::persist_claim(context, consumer, entry);
    require((co_await channel::completion(context.redis(), entry.report_id_)) == true, "delayed heartbeat commits using its original arrival time");
    const auto timestamp = co_await context.db().query("SELECT floor(extract(epoch FROM last_heartbeat_at)*1000)::bigint,last_heartbeat_at >= now()-interval '15 seconds' FROM sys_node WHERE id=$1", node);
    require(timestamp[0][0].as<std::int64_t>() == received && timestamp[0][1].as<bool>() == false,
        "a late heartbeat cannot revive an expired online window");
    report = heartbeat(now_ms());
    report.envelope_.mutable_heartbeat()->set_cpu_usage(0.75);
    entry = co_await append(context, report);
    co_await channel::persist_claim(context, consumer, entry);
    auto older = heartbeat(received - 1000);
    older.envelope_.mutable_heartbeat()->set_cpu_usage(0.1);
    const auto older_entry = co_await append(context, older);
    co_await channel::persist_claim(context, consumer, older_entry);
    const auto newer = co_await context.db().query("SELECT runtime->>'cpu_usage',floor(extract(epoch FROM last_heartbeat_at)*1000)::bigint FROM sys_node WHERE id=$1", node);
    require(newer[0][0].value() == "0.75" && newer[0][1].as<std::int64_t>() == report.received_unix_ms_,
        "out-of-order reports preserve the newer runtime and timestamp");
    const auto revoked = co_await append(context, heartbeat(now_ms() + 1));
    (void)co_await context.db().execute("UPDATE sys_node SET node_secret_hash=repeat('d',64) WHERE id=$1", node);
    co_await channel::persist_claim(context, consumer, revoked);
    require((co_await channel::completion(context.redis(), revoked.report_id_)) == false,
        "persistence rechecks the current DB credential after Redis accepted an old projection");
    (void)co_await context.db().execute("UPDATE sys_node SET node_secret_hash=$1 WHERE id=$2", hash, node);
    (void)co_await channel::clean_report_receipts(context);
}

ruvia::task<void> verify(report_context& context, report_context& peer) {
    co_await fixture(context);
    co_await verify_queue(context);
    co_await verify_authentication_projection(context);
    co_await verify_apply_transaction(context);
    co_await verify_heartbeat(context);
    co_await verify_delayed_owner(context, peer);
    co_await verify_redis_reset_collision(context);
    co_await verify_worker_loop(context);
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
        service::utils::configure_secret_key(std::string(64, 'a'));
        const ruvia::db_migrator migrator(temporary_database.config(), {.table_ = "schema_migrations"});
        (void)migrator.migrate(service::config::schema_migrations);
        ruvia::event_loop_pool loops({.loop_count_ = 1});
        const auto worker = loops.loop(0);
        ruvia::db_client database(worker, temporary_database.config());
        ruvia::db_client peer_database(worker, temporary_database.config());
        ruvia::redis_client redis(worker, *redis_config);
        report_context context{.database_ = database, .redis_ = redis, .worker_ = worker.handle(), .claims_ = {}, .stop_ = {}};
        report_context peer{.database_ = peer_database, .redis_ = redis, .worker_ = worker.handle(), .claims_ = {}, .stop_ = {}};
        loops.start();
        std::exception_ptr failure;
        try {
            worker.start(database.connect()).get();
            worker.start(peer_database.connect()).get();
            worker.start(redis.connect()).get();
            worker.start(verify(context, peer)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            worker.start(database.shutdown()).get();
            worker.start(peer_database.shutdown()).get();
            worker.start(redis.shutdown()).get();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        database.close();
        peer_database.close();
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
    std::cout << "Node reports: real Redis identity/budget/fencing, PostgreSQL rollback/replay/trace, bounded ledger cleanup, stale heartbeat and revoked credentials passed\n";
    return 0;
}
