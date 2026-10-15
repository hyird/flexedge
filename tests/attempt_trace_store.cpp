#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/model_object.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/config/schema.h"
#include "service/domains/task/task_service.h"
#include "service/features/attempt_trace/recorder.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/sync_runtime/state.h"
#include "support/attempt_trace_fixture.h"
#include "support/operation_fixture.h"
#include "support/postgres_test.h"
#include "support/recorded_notifications.h"

namespace {

using service::attempt_trace::outbound_operation;
using service::attempt_trace::step_code;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("attempt trace store: " + std::string(message));
    }
}

RUVIA_MODEL(summary_step_input, RUVIA_REQUIRED_FIELD(code, ruvia::string), RUVIA_REQUIRED_FIELD(label, ruvia::string),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string), RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(count, ruvia::int64));
RUVIA_MODEL(failed_step_input, RUVIA_REQUIRED_FIELD(code, ruvia::string), RUVIA_REQUIRED_FIELD(label, ruvia::string));
RUVIA_MODEL(summary_input, RUVIA_REQUIRED_FIELD(state, ruvia::string), RUVIA_REQUIRED_FIELD(attempt_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(step_count, ruvia::int64), RUVIA_REQUIRED_FIELD(call_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(log_count, ruvia::int64), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(failed_step, failed_step_input, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(steps, ruvia::array<summary_step_input>));

constexpr std::string_view fresh_database = "attempt_trace_fresh_auth_qa";
constexpr std::string_view upgrade_database = "attempt_trace_upgrade_auth_qa";
constexpr std::string_view drift_database = "attempt_trace_drift_auth_qa";
constexpr std::string_view worker = "attempt-trace-qa";
constexpr std::string_view tenant = "00000000-0000-4000-8000-0000000001a1";
constexpr std::string_view other_tenant = "00000000-0000-4000-8000-0000000001a2";
constexpr std::string_view provider = "00000000-0000-4000-8000-0000000001b1";
constexpr std::string_view zone = "00000000-0000-4000-8000-0000000001c1";
constexpr std::string_view marker = "00000000-0000-4000-8000-000000000171";
constexpr std::string_view legacy_task = "00000000-0000-4000-8000-000000000161";
constexpr std::string_view legacy_resource = "00000000-0000-4000-8000-000000000162";

std::vector<std::string> canaries;
// The task and subtask doing the marker's DNS sync, and a task of the same
// tenant that does other work.
service::operation::work_owner dns_owner;
service::operation::work_owner provider_owner;

using read_context = test_support::read_context<ruvia::db_handle>;

ruvia::db_config with_database(const ruvia::db_config& base, std::string_view database) {
    auto config = base;
    config.database_ = std::string(database);
    return config;
}

std::int64_t integer(const ruvia::db_rows& rows, std::size_t column = 0) {
    return rows.front()[column].as<std::int64_t>().value_or(-1);
}

std::string text(const ruvia::db_rows& rows, std::size_t column = 0) {
    return std::string(rows.front()[column].value().value_or(""));
}

// Migrations up to `last` (inclusive).
std::vector<ruvia::db_migration> migrations_through(std::string_view last) {
    const auto& all = service::config::schema_migrations;
    for (std::size_t index = 0; index < all.size(); ++index) {
        if (all[index].id() == last) {
            return {all.begin(), all.begin() + static_cast<std::ptrdiff_t>(index + 1)};
        }
    }
    throw std::runtime_error("attempt trace store: unknown migration " + std::string(last));
}

int migrate(const ruvia::db_config& config, std::span<const ruvia::db_migration> migrations,
    std::size_t expected_applied, std::string_view label) {
    const ruvia::db_migrator migrator(config, {.table_ = "schema_migrations"});
    const auto report = migrator.migrate(migrations);
    if (report.applied().size() != expected_applied) {
        std::cerr << label << ": applied " << report.applied().size() << ", expected " << expected_applied << '\n';
        return 1;
    }
    return 0;
}

ruvia::task<void> recreate_databases(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    for (const auto database : {fresh_database, upgrade_database, drift_database}) {
        (void)co_await db.execute("DROP DATABASE IF EXISTS " + std::string(database) + " WITH (FORCE)");
        (void)co_await db.execute("CREATE DATABASE " + std::string(database));
    }
}

ruvia::task<void> verify_schema(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto columns = co_await db.query(
        "SELECT string_agg(column_name || ':' || data_type || ':' || is_nullable, ',' ORDER BY column_name) "
        "FROM information_schema.columns WHERE table_name = 'sys_sync_event_trace'");
    require(text(columns) ==
                "created_at:timestamp with time zone:NO,event_id:bigint:NO,summary:jsonb:NO,tenant_id:uuid:NO,"
                "trace:jsonb:NO,trace_bytes:integer:NO",
        "trace columns: " + text(columns));
    const auto constraints = co_await db.query(
        "SELECT string_agg(conname || ':' || contype::text || ':' || confdeltype::text, ',' ORDER BY conname) FROM "
        "pg_constraint "
        "WHERE conrelid = 'public.sys_sync_event_trace'::regclass");
    for (const auto* expected : {"ck_sync_event_trace_bytes:c", "ck_sync_event_trace_document:c",
             "ck_sync_event_trace_summary:c", "fk_sync_event_trace_event:f:c", "fk_sync_event_trace_tenant:f:a"}) {
        require(text(constraints).find(expected) != std::string::npos,
            std::string("constraint ") + expected + " in " + text(constraints));
    }
}

ruvia::task<void> insert_legacy_rows(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute(
        "INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'Trace QA', 'trace-qa'), ($2, 'Trace other', 'trace-other')",
        tenant, other_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, outcome, "
        "error, attempt) VALUES ($1, $2, 'dns_zone', $3, 'sync', 1, 'failed', 'before traces', 1)",
        tenant, legacy_task, legacy_resource);
}

ruvia::task<void> insert_fixture(ruvia::db_handle& db) {
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id) VALUES "
        "($1, $2, 'dns', 'cloudflare', 'Trace DNS', 'qa-account')",
        provider, tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain) VALUES ($1, $2, $3, 'trace.example.com')", zone,
        tenant, provider);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, dns_zone_id, operation, version) VALUES "
        "($1, $2, 'dns_zone', $3, 'sync', 1)",
        marker, tenant, zone);
    dns_owner = co_await test_support::own_work(db, tenant, service::operation::action::dns_zone_sync, "dns_zone",
        zone, service::operation::subtask_kind::dns_sync, "dns_zone", zone, 1, marker);
    provider_owner = co_await test_support::own_work(db, tenant, service::operation::action::provider_verify,
        "provider", provider, service::operation::subtask_kind::provider_verify, "provider", provider, 1);
    require(!dns_owner.empty() && !provider_owner.empty(), "fixture tasks");
}

// One failed attempt through the production path: the marker transition and
// attempt row commit, then the trace is stored after the commit.
ruvia::task<std::optional<std::int64_t>> record_attempt(ruvia::db_handle& db, std::string_view error) {
    (void)co_await db.execute(
        "UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds' WHERE id = $1", marker,
        worker);
    const auto started = std::chrono::steady_clock::now() - std::chrono::milliseconds(250);
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "dns-sync", started);
    const auto token = test_support::attempt_trace::canary("token");
    canaries.push_back(token);
    trace.register_secret(token);
    {
        auto step = trace.begin_step(step_code::find_zone, "trace.example.com");
        test_support::attempt_trace::perform(trace,
            {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                 .operation_ = outbound_operation::cloudflare_find_zone,
                 .subject_ = "trace.example.com"},
                .target_ = "/client/v4/zones?name=trace.example.com&status=active&per_page=5",
                .request_headers_ = {{"authorization", "Bearer " + token}},
                .failure_ = ruvia::http_client_error::code_type::connect_failed});
        trace.log(service::attempt_trace::log_level::warning, "echo " + token);
    }
    trace.mark_failure(error);
    auto step = trace.begin_step(step_code::record_failure);
    const auto lease = service::sync_runtime::make_running_lease(tenant, marker, 1, worker, started,
        service::sync_runtime::marker_resource_type::dns_zone, zone);
    auto transaction = co_await db.begin_transaction();
    const auto transition = co_await service::sync_runtime::fail_running_and_record_event(transaction, lease, error,
        R"({"kind":"dns_sync","truncated":false})");
    test_support::recorded_notifications worker{.handle_ = db};
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, worker, lease, transition, trace);
    require(worker.sent_.size() == 1 && worker.committed_[0], "result notified after its commit");
    step.complete();
    require(transition.marker_transitioned_ && transition.event_recorded_ && transition.event_id_.has_value(),
        "attempt recorded with its id");
    co_return transition.event_id_;
}

ruvia::task<std::int64_t> trace_count(ruvia::db_handle& db) {
    const auto rows = co_await db.query(
        "SELECT count(*) FROM sys_sync_event_trace stored JOIN sys_sync_event event ON event.id = stored.event_id "
        "WHERE event.task_id = $1",
        marker);
    co_return integer(rows);
}

ruvia::task<void> verify_node_apply_trace(ruvia::db_handle& db, read_context& context) {
    constexpr std::string_view cluster = "00000000-0000-4000-8000-0000000001e1";
    constexpr std::string_view node = "00000000-0000-4000-8000-000000000191";
    constexpr std::string_view release = "00000000-0000-4000-8000-000000000181";
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(id, tenant_id, dns_zone_id, name, hostname_prefix) VALUES "
        "($1, $2, $3, 'Trace cluster', 'edge')",
        cluster, tenant, zone);
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, runtime, agent_id, node_secret_hash, "
        "node_secret_envelope) VALUES ($1, $2, $3, 'Trace node', '{\"agent_version\":\"0.4.1\"}', repeat('e', 32), "
        "repeat('f', 64), 'qa-only')",
        node, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster_release(id, tenant_id, cluster_id, generation, status, manifest_digest, "
        "manifest_envelope, schema_version) VALUES ($1, $2, $3, 7, 'active', repeat('a', 64), 'qa-only', 6)",
        release, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_node_release_target(tenant_id, release_id, node_id, status) VALUES ($1, $2, $3, 'pending')",
        tenant, release, node);

    // What the control connection records: its own messages plus the steps
    // and log lines the node reported in ApplyResult.
    const auto bearer = test_support::attempt_trace::canary("nodebearer");
    canaries.push_back(bearer);
    const auto now = std::chrono::system_clock::now();
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::node_apply, "web",
        std::chrono::steady_clock::now() - std::chrono::milliseconds(300));
    const auto validate_started = now - std::chrono::milliseconds(150);
    trace.record_reported_step(service::attempt_trace::node_phase_step("stage"), "stage", "",
        now - std::chrono::milliseconds(200), 5, true, {});
    trace.record_reported_step(service::attempt_trace::node_phase_step("validate"), "validate", "", validate_started, 40,
        false, "upstream pool web-1: no healthy origin");
    // Written after stage ended, at the very millisecond validate started.
    trace.record_reported_log(service::attempt_trace::log_level::info, validate_started, "stage",
        "staged the release state");
    trace.record_reported_log(service::attempt_trace::log_level::error, now - std::chrono::milliseconds(110), "validate",
        "apply failed: Authorization: Bearer " + bearer);
    auto result_step = trace.begin_step(step_code::record_result);
    auto transaction = co_await db.begin_transaction();
    const auto recorded = co_await service::node_dispatch::record_node_apply_attempt(transaction,
        {.tenant_id_ = tenant,
            .release_id_ = release,
            .node_id_ = node,
            .source_ = "apply_result",
            .applied_ = false,
            .phase_ = "validate",
            .error_code_ = "artifact_invalid",
            .error_ = "upstream pool web-1: no healthy origin",
            .retryable_ = false,
            .elapsed_milliseconds_ = 300},
        context.pool());
    co_await transaction.commit();
    require(recorded.has_value() && recorded->version_ == 7, "node attempt id and release generation");
    // The unowned target's failure is adopted by the system task 「后台任务」.
    const auto node_owner = recorded->owner_;
    require(!node_owner.empty(), "failed unowned node work belongs to a system task");
    co_await service::attempt_trace::store_attempt_trace(db,
        {.tenant_id_ = tenant, .task_id_ = release, .resource_id_ = node, .version_ = recorded->version_},
        recorded->event_id_, trace);
    result_step.complete();

    auto& tasks = service::task::get_task_service();
    const auto history =
        co_await tasks.history(context, std::string(tenant), node_owner.operation_id_, node_owner.subtask_id_);
    const auto summary = ruvia::from_json<summary_input>(history.get<"list">()[0].get<"trace">().view());
    require(summary && summary->get<"state">().view() == "available" &&
                summary->get<"failed_step">()->get<"code">().view() == "validate_config" &&
                summary->get<"steps">().size() == 3 && summary->get<"steps">()[0].get<"code">().view() == "stage_config" &&
                summary->get<"steps">()[2].get<"code">().view() == "record_result",
        "node apply summary orders reported steps by start");
    const auto full =
        co_await tasks.attempt_trace(context, std::string(tenant), node_owner.operation_id_, recorded->event_id_);
    const auto json = std::string(ruvia::to_json(full));
    require(full.get<"kind">().view() == "node_apply" && full.get<"worker_type">().view() == "web" &&
                full.get<"resource_type">().view() == "node" &&
                json.find("no healthy origin") != std::string::npos &&
                json.find("Bearer [REDACTED]") != std::string::npos,
        "node apply trace stored and read: " + json.substr(0, 300));
    const auto stage_at = json.find("stage_config");
    const auto staged_at = json.find("staged the release state");
    const auto validate_at = json.find("validate_config");
    const auto failed_at = json.find("apply failed:");
    require(stage_at < staged_at && staged_at < validate_at && validate_at < failed_at && failed_at != std::string::npos,
        "stored node log lines sit under the step of their phase, also on a timestamp tie");
}

ruvia::task<void> verify_records(ruvia::db_client& client) {
    test_support::attempt_trace::ensure_outbound_origins();
    co_await client.connect();
    auto db = client.handle();
    std::pmr::monotonic_buffer_resource pool;
    read_context context{.handle_ = client.handle(), .pool_ = &pool};
    co_await insert_fixture(db);
    auto& tasks = service::task::get_task_service();

    // Attempts retained before migration 0025 were removed with their traces.
    const auto legacy_rows = co_await db.query("SELECT count(*) FROM sys_sync_event WHERE task_id = $1", legacy_task);
    require(integer(legacy_rows) == 0, "attempts recorded before tasks are removed by 0025");

    // A recorded attempt carries its summary in the history and its full trace.
    const auto first = co_await record_attempt(db, "Cloudflare API 连接失败");
    const auto history = co_await tasks.history(context, std::string(tenant), dns_owner.operation_id_,
        dns_owner.subtask_id_);
    require(history.get<"list">().size() == 1, "one attempt");
    const auto summary = ruvia::from_json<summary_input>(history.get<"list">()[0].get<"trace">().view());
    require(summary.has_value(), "trace summary matches its contract");
    require(summary->get<"state">().view() == "available" &&
                summary->get<"attempt_id">().view() == std::to_string(*first) &&
                summary->get<"failed_step">()->get<"code">().view() == "find_zone" &&
                summary->get<"call_count">().value_ == 1 && summary->get<"steps">().size() == 2,
        "summary of the stored trace");
    const auto full = co_await tasks.attempt_trace(context, std::string(tenant), dns_owner.operation_id_, *first);
    const auto json = std::string(ruvia::to_json(full));
    require(full.get<"state">().view() == "available" && full.get<"kind">().view() == "dns_sync" &&
                full.get<"worker_type">().view() == "dns-sync" && full.get<"attempt">().value_ == 1 &&
                full.get<"outcome">().view() == "failed" && full.get<"resource_type">().view() == "dns_zone" &&
                full.get<"subtask_id">().view() == dns_owner.subtask_id_ && full.get<"steps">().is_array() &&
                json.find("connect_failed") != std::string::npos && json.find("Bearer") == std::string::npos,
        "full trace joined with its attempt: " + json.substr(0, 300));

    // Tenant and task scoping.
    bool other_tenant_rejected = false;
    try {
        (void)co_await tasks.attempt_trace(context, std::string(other_tenant), dns_owner.operation_id_, *first);
    } catch (const std::exception&) {
        other_tenant_rejected = true;
    }
    require(other_tenant_rejected, "another tenant cannot read the trace");
    bool other_task_rejected = false;
    try {
        (void)co_await tasks.attempt_trace(context, std::string(tenant), provider_owner.operation_id_, *first);
    } catch (const std::exception&) {
        other_task_rejected = true;
    }
    require(other_task_rejected, "the attempt must belong to a subtask of the task");

    // Retention: the newest attempts and the first one keep their traces.
    std::vector<std::int64_t> ids{*first};
    for (int attempt = 0; attempt < 24; ++attempt) {
        ids.push_back(*co_await record_attempt(db, "Cloudflare API 连接失败"));
    }
    require(co_await trace_count(db) == service::attempt_trace::retained_traces + 1, "20 newest plus the first");
    const auto kept = co_await db.query(
        "SELECT (SELECT count(*) FROM sys_sync_event_trace WHERE event_id = $1), "
        "(SELECT count(*) FROM sys_sync_event_trace WHERE event_id = $2), "
        "(SELECT count(*) FROM sys_sync_event_trace WHERE event_id = $3), "
        "(SELECT count(*) FROM sys_sync_event WHERE task_id = $4)",
        ids[0], ids[1], ids.back(), marker);
    require(integer(kept, 0) == 1 && integer(kept, 1) == 0 && integer(kept, 2) == 1 && integer(kept, 3) == 25,
        "older traces dropped, attempt rows kept");

    // Nothing secret reached the table.
    const auto stored = co_await db.query("SELECT string_agg(summary::text || trace::text, '') FROM sys_sync_event_trace");
    test_support::attempt_trace::require_no_canary(text(stored), canaries, "sys_sync_event_trace");
    require(text(stored).find("Bearer") == std::string::npos && text(stored).find("[REDACTED]") != std::string::npos,
        "stored traces are redacted");

    // A trace that cannot be stored changes nothing about the attempt.
    (void)co_await db.execute(
        "ALTER TABLE sys_sync_event_trace ADD CONSTRAINT ck_forced_failure CHECK (event_id < 0) NOT VALID");
    const auto before = co_await db.query("SELECT count_fails FROM sys_sync_task WHERE id = $1", marker);
    const auto isolated = co_await record_attempt(db, "forced trace failure");
    const auto after = co_await db.query(
        "SELECT count_fails, lease_owner IS NULL AND lease_until IS NULL, next_attempt_at > NOW() + INTERVAL '10 "
        "seconds', error, (SELECT count(*) FROM sys_sync_event_trace WHERE event_id = $2), (SELECT outcome FROM "
        "sys_sync_event WHERE id = $2) FROM sys_sync_task WHERE id = $1",
        marker, *isolated);
    require(integer(after, 0) == integer(before) + 1 && after.front()[1].as<bool>().value_or(false) &&
                after.front()[2].as<bool>().value_or(false) && text(after, 3) == "forced trace failure" &&
                integer(after, 4) == 0 && text(after, 5) == "failed",
        "failure, lease release and retry schedule unchanged without a stored trace");
    const auto isolated_history =
        co_await tasks.history(context, std::string(tenant), dns_owner.operation_id_, dns_owner.subtask_id_);
    const auto isolated_summary =
        ruvia::from_json<summary_input>(isolated_history.get<"list">()[0].get<"trace">().view());
    require(isolated_summary && isolated_summary->get<"state">().view() == "none" &&
                isolated_summary->get<"attempt_id">().view() == std::to_string(*isolated) &&
                !isolated_summary->get<"failed_step">().has_value(),
        "history shows the attempt without a trace");
    const auto untraced = co_await tasks.attempt_trace(context, std::string(tenant), dns_owner.operation_id_, *isolated);
    require(untraced.get<"state">().view() == "none" && untraced.get<"kind">().view() == "dns_sync" &&
                untraced.get<"worker_type">().view() == "dns-sync" && untraced.get<"steps">().view() == "[]" &&
                untraced.get<"attempt">().value_ == 26,
        "an attempt without a stored trace reads as none");
    (void)co_await db.execute("ALTER TABLE sys_sync_event_trace DROP CONSTRAINT ck_forced_failure");

    co_await verify_node_apply_trace(db, context);

    // Retention of the history removes the traces with their attempts.
    (void)co_await db.execute(
        "UPDATE sys_sync_event SET emitted_at = NOW() - INTERVAL '8 days' WHERE tenant_id = $1", tenant);
    test_support::recorded_notifications pruner{.handle_ = db};
    co_await service::sync_runtime::prune_result_events(pruner);
    require(co_await trace_count(db) == 0, "traces follow the 7 day history retention");
    const auto orphaned = co_await db.query("SELECT count(*) FROM sys_sync_event_trace");
    require(integer(orphaned) == 0, "no trace without its attempt");
}

ruvia::task<void> create_conflicting_table(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute("CREATE TABLE sys_sync_event_trace (event_id text)");
}

ruvia::task<void> verify_drift_rejected(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto recorded = co_await db.query(
        "SELECT count(*) FILTER (WHERE migration_id = '0024_task_attempt_traces'), "
        "count(*) FILTER (WHERE migration_id = '0023_task_attempt_details') FROM schema_migrations");
    require(integer(recorded, 0) == 0 && integer(recorded, 1) == 1, "0024 not recorded after the conflict");
    const auto column = co_await db.query(
        "SELECT data_type FROM information_schema.columns WHERE table_name = 'sys_sync_event_trace' AND column_name = "
        "'event_id'");
    require(text(column) == "text", "conflicting table left untouched");
}

}  // namespace

int main() {
    std::optional<ruvia::db_config> base;
    try {
        base = test_support::isolated_postgres_config();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (!base) {
        return 77;
    }
    const auto fresh = with_database(*base, fresh_database);
    const auto upgrade = with_database(*base, upgrade_database);
    const auto drift = with_database(*base, drift_database);
    const auto& all = service::config::schema_migrations;
    try {
        const auto before = migrations_through("0023_task_attempt_details");
        const auto through_traces = migrations_through("0024_task_attempt_traces");
        if (test_support::run_postgres_test(*base, recreate_databases) != 0 ||
            migrate(fresh, all, all.size(), "fresh install") != 0 || migrate(fresh, all, 0, "fresh repeat") != 0 ||
            test_support::run_postgres_test(fresh, verify_schema) != 0 ||
            migrate(upgrade, before, before.size(), "0023 install") != 0 ||
            test_support::run_postgres_test(upgrade, insert_legacy_rows) != 0 ||
            migrate(upgrade, through_traces, 1, "0024 upgrade") != 0 ||
            test_support::run_postgres_test(upgrade, verify_schema) != 0 ||
            migrate(upgrade, all, all.size() - through_traces.size(), "0025 upgrade") != 0 ||
            migrate(upgrade, all, 0, "upgrade repeat") != 0 ||
            test_support::run_postgres_test(upgrade, verify_schema) != 0 ||
            test_support::run_postgres_test(upgrade, verify_records) != 0 ||
            migrate(drift, before, before.size(), "drift install") != 0 ||
            test_support::run_postgres_test(drift, create_conflicting_table) != 0) {
            return 1;
        }
        bool rejected = false;
        try {
            const ruvia::db_migrator migrator(drift, {.table_ = "schema_migrations"});
            (void)migrator.migrate(all);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "a conflicting sys_sync_event_trace rejects 0024");
        if (test_support::run_postgres_test(drift, verify_drift_rejected) != 0) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Attempt trace migration, storage, reads and failure isolation passed\n";
    return 0;
}
