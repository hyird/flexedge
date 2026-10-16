#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
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
#include "service/features/certificate/dns_challenge.h"
#include "service/features/certificate/queue.h"
#include "service/features/cluster_dns/projection.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/node_dispatch/queue.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "support/operation_fixture.h"
#include "support/postgres_test.h"
#include "support/recorded_notifications.h"

// Tasks as operations (docs/task-center.md)
// on the real schema: migration 0025 (fresh install, upgrade with retained
// rows, repeated run, rejected drift), a server without task tracking on the
// new schema, ownership through markers and release targets (merge, cancel,
// completed work), status derivation and roll-up, the system task adopting
// unowned failures, retention, tenant isolation, the detail stream's attempt
// reads and notifications, and the DNS-01 children of a certificate issuance.
namespace {

using service::operation::action;
using service::operation::subtask_kind;
using service::operation::work_origin;
using service::operation::work_owner;
using service::sync_runtime::marker_operation;
using service::sync_runtime::marker_resource_type;
using read_context = test_support::read_context<ruvia::db_handle>;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("operation tasks: " + std::string(message));
    }
}

constexpr std::string_view fresh_database = "operation_tasks_fresh_auth_qa";
constexpr std::string_view upgrade_database = "operation_tasks_upgrade_auth_qa";
constexpr std::string_view drift_database = "operation_tasks_drift_auth_qa";
constexpr std::string_view worker = "operation-tasks-qa";

constexpr std::string_view tenant = "00000000-0000-4000-8000-0000000003a1";
constexpr std::string_view other_tenant = "00000000-0000-4000-8000-0000000003a2";
constexpr std::string_view admin = "00000000-0000-4000-8000-0000000003a3";
constexpr std::string_view dns_provider = "00000000-0000-4000-8000-0000000003b1";
constexpr std::string_view certificate_provider = "00000000-0000-4000-8000-0000000003b2";
constexpr std::string_view other_provider = "00000000-0000-4000-8000-0000000003b3";
constexpr std::string_view cluster_zone = "00000000-0000-4000-8000-0000000003c0";
constexpr std::string_view other_zone = "00000000-0000-4000-8000-0000000003cf";
constexpr std::string_view cluster = "00000000-0000-4000-8000-0000000003e1";
constexpr std::string_view failing_cluster = "00000000-0000-4000-8000-0000000003e2";
constexpr std::string_view partial_cluster = "00000000-0000-4000-8000-0000000003e3";
constexpr std::string_view orphan_cluster = "00000000-0000-4000-8000-0000000003e4";
constexpr std::string_view node_online = "00000000-0000-4000-8000-000000000391";
constexpr std::string_view node_offline = "00000000-0000-4000-8000-000000000392";
constexpr std::string_view node_spare = "00000000-0000-4000-8000-000000000393";
constexpr std::string_view node_failing = "00000000-0000-4000-8000-000000000394";
constexpr std::string_view node_partial = "00000000-0000-4000-8000-000000000395";
constexpr std::string_view node_orphan = "00000000-0000-4000-8000-000000000396";
constexpr std::string_view certificate = "00000000-0000-4000-8000-0000000003d1";

// Zone `index` (1-17) of the tenant: z<index>.example.com.
std::string zone(int index) {
    const auto suffix = std::to_string(index);
    return "00000000-0000-4000-8000-0000000003" + std::string(2 - std::min<std::size_t>(2, suffix.size()), '0') +
           suffix;
}

std::string zone_domain(int index) {
    return "z" + std::to_string(index) + ".example.com";
}

ruvia::db_config with_database(const ruvia::db_config& base, std::string_view database) {
    auto config = base;
    config.database_ = std::string(database);
    return config;
}

std::vector<ruvia::db_migration> migrations_through(std::string_view last) {
    const auto& all = service::config::schema_migrations;
    const auto found =
        std::find_if(all.begin(), all.end(), [&](const auto& migration) { return migration.id() == last; });
    require(found != all.end(), std::string(last) + " is part of the migrations");
    return {all.begin(), found + 1};
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

std::string text(const ruvia::db_rows& rows, std::size_t column = 0) {
    return rows.empty() ? std::string{} : std::string(rows.front()[column].value().value_or(""));
}

std::int64_t integer(const ruvia::db_rows& rows, std::size_t column = 0) {
    return rows.empty() ? -1 : rows.front()[column].as<std::int64_t>().value_or(-1);
}

service::task::task_service& tasks() {
    return service::task::get_task_service();
}

work_origin user(action value, std::string_view target_type, std::string_view target_id, std::string summary = "{}") {
    return work_origin::start({.action_ = value,
        .actor_id_ = std::string(admin),
        .target_type_ = std::string(target_type),
        .target_id_ = std::string(target_id),
        .summary_ = std::move(summary)});
}

ruvia::task<void> recreate_databases(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    for (const auto database : {fresh_database, upgrade_database, drift_database}) {
        (void)co_await db.execute("DROP DATABASE IF EXISTS " + std::string(database) + " WITH (FORCE)");
        (void)co_await db.execute("CREATE DATABASE " + std::string(database));
    }
}

// ---------------------------------------------------------------------------
// Migration 0025

ruvia::task<void> verify_schema(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto owners = co_await db.query(
        "SELECT string_agg(table_name || '.' || column_name || ':' || data_type || ':' || is_nullable, ',' ORDER "
        "BY table_name) FROM information_schema.columns WHERE table_schema = 'public' AND column_name = "
        "'subtask_id'");
    require(text(owners) ==
                "sys_node_release_target.subtask_id:uuid:YES,sys_sync_event.subtask_id:uuid:YES,"
                "sys_sync_task.subtask_id:uuid:YES",
        "nullable owner columns without default: " + text(owners));
    const auto defaults = co_await db.query(
        "SELECT count(*) FROM information_schema.columns WHERE column_name = 'subtask_id' AND column_default IS NOT "
        "NULL");
    require(integer(defaults) == 0, "owner columns have no default");
    const auto tables = co_await db.query(
        "SELECT string_agg(table_name, ',' ORDER BY table_name) FROM information_schema.tables WHERE table_name IN "
        "('sys_operation', 'sys_operation_subtask')");
    require(text(tables) == "sys_operation,sys_operation_subtask", "task tables: " + text(tables));
    const auto unique_owner = co_await db.query("SELECT indexdef FROM pg_indexes WHERE indexname = 'uk_sync_task_subtask'");
    require(text(unique_owner).find("UNIQUE") != std::string::npos &&
                text(unique_owner).find("subtask_id IS NOT NULL") != std::string::npos,
        "one marker per subtask: " + text(unique_owner));
    const auto actions = co_await db.query(
        "SELECT string_agg(conname || ':' || confdeltype::text, ',' ORDER BY conname) FROM pg_constraint WHERE "
        "conname IN ('fk_sync_task_subtask', 'fk_node_release_target_subtask', 'fk_sync_event_subtask', "
        "'fk_operation_subtask_operation', 'fk_operation_subtask_parent', 'fk_operation_subtask_merged_into')");
    require(text(actions) ==
                "fk_node_release_target_subtask:n,fk_operation_subtask_merged_into:n,fk_operation_subtask_operation:c,"
                "fk_operation_subtask_parent:c,fk_sync_event_subtask:n,fk_sync_task_subtask:n",
        "deleting a task releases its work and removes its subtasks: " + text(actions));
}

// Scheduling state a 0024 database holds, plus retained history.
ruvia::task<void> insert_upgrade_rows(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute("INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'Upgrade QA', 'upgrade-qa')", tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id, status) VALUES ($1, $2, 'dns', "
        "'cloudflare', 'Upgrade DNS', 'acct', 'verified')",
        dns_provider, tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, sync_status, desired_revision, synced_revision) "
        "VALUES ($1, $3, $4, 'z1.example.com', 'synced', 3, 3), ($2, $3, $4, 'z2.example.com', 'failed', 5, 4)",
        zone(1), zone(2), tenant, dns_provider);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(id, tenant_id, dns_zone_id, name, hostname_prefix, release_generation) VALUES ($1, "
        "$2, $3, 'Upgrade cluster', 'edge', 2)",
        cluster, tenant, zone(1));
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, registration_status, registered_at, agent_id, "
        "node_secret_hash, node_secret_envelope) VALUES ($1, $3, $4, 'Upgrade A', 'registered', NOW(), repeat('a', "
        "32), repeat('c', 64), 'qa-only'), ($2, $3, $4, 'Upgrade B', 'registered', NOW(), repeat('b', 32), "
        "repeat('d', 64), 'qa-only')",
        node_online, node_offline, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster_release(id, tenant_id, cluster_id, generation, status, manifest_digest, "
        "manifest_envelope, schema_version) VALUES ('00000000-0000-4000-8000-000000000381', $1, $2, 1, "
        "'superseded', repeat('a', 64), 'qa-only', 6), ('00000000-0000-4000-8000-000000000382', $1, $2, 2, "
        "'active', repeat('b', 64), 'qa-only', 6)",
        tenant, cluster);
    (void)co_await db.execute(
        "UPDATE sys_cluster SET current_release_id = '00000000-0000-4000-8000-000000000382' WHERE id = $1", cluster);
    (void)co_await db.execute(
        "UPDATE sys_node SET desired_release_id = '00000000-0000-4000-8000-000000000382' WHERE cluster_id = $1",
        cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_node_release_target(tenant_id, release_id, node_id, status, retryable, last_error) VALUES "
        "($1, '00000000-0000-4000-8000-000000000381', $2, 'applied', NULL, NULL), ($1, "
        "'00000000-0000-4000-8000-000000000381', $3, 'failed', TRUE, 'old failure'), ($1, "
        "'00000000-0000-4000-8000-000000000382', $2, 'pending', NULL, NULL), ($1, "
        "'00000000-0000-4000-8000-000000000382', $3, 'applied', NULL, NULL)",
        tenant, node_online, node_offline);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, dns_zone_id, operation, version, processed_version, "
        "is_done, is_ok, count_fails, error) VALUES ('00000000-0000-4000-8000-000000000371', $1, 'dns_zone', $2, "
        "'sync', 3, 3, TRUE, TRUE, 0, ''), ('00000000-0000-4000-8000-000000000372', $1, 'dns_zone', $3, 'sync', 5, "
        "0, FALSE, FALSE, 2, 'still failing')",
        tenant, zone(1), zone(2));
    const auto events = co_await db.query(
        "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, outcome, "
        "error, attempt) VALUES ($1, '00000000-0000-4000-8000-000000000371', 'dns_zone', $2, 'sync', 3, "
        "'completed', '', 1), ($1, '00000000-0000-4000-8000-000000000372', 'dns_zone', $3, 'sync', 5, 'failed', "
        "'still failing', 1), ($1, '00000000-0000-4000-8000-000000000381', 'node', $4, 'publish', 1, 'failed', 'old "
        "failure', 1) RETURNING id",
        tenant, zone(1), zone(2), node_offline);
    for (std::size_t index = 0; index < 2; ++index) {
        (void)co_await db.execute(
            "INSERT INTO sys_sync_event_trace(event_id, tenant_id, summary, trace, trace_bytes) VALUES ($1, $2, "
            "'{}', '{}', 2)",
            events[index][0].as<std::int64_t>().value_or(0), tenant);
    }
}

// Content fingerprints of the scheduling state that must survive 0025.
ruvia::task<std::string> scheduling_fingerprint(ruvia::db_handle& db) {
    const auto rows = co_await db.query(
        "SELECT (SELECT md5(string_agg(id::text || resource_type || resource_id::text || operation || version || "
        "processed_version || is_done || is_ok || count_fails || error, ',' ORDER BY id)) FROM sys_sync_task) || "
        "(SELECT md5(string_agg(target.release_id::text || target.node_id::text || target.status, ',' ORDER BY "
        "target.release_id, target.node_id)) FROM sys_node_release_target target JOIN sys_cluster_release release "
        "ON release.id = target.release_id WHERE release.status <> 'superseded') || (SELECT "
        "md5(string_agg(id::text || generation || status || manifest_digest, ',' ORDER BY id)) FROM "
        "sys_cluster_release) || (SELECT md5(string_agg(id::text || sync_status || desired_revision || "
        "synced_revision, ',' ORDER BY id)) FROM sys_dns_zone) || (SELECT md5(string_agg(id::text || "
        "desired_release_id::text || registration_status, ',' ORDER BY id)) FROM sys_node) || (SELECT "
        "current_release_id::text FROM sys_cluster)");
    co_return text(rows);
}

std::string upgrade_fingerprint;

ruvia::task<void> record_upgrade_fingerprint(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    upgrade_fingerprint = co_await scheduling_fingerprint(db);
    require(upgrade_fingerprint.size() == 5 * 32 + 36, "fingerprint covers every scheduling table");
}

ruvia::task<void> verify_upgraded_rows(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto counts = co_await db.query(
        "SELECT (SELECT count(*) FROM sys_sync_event), (SELECT count(*) FROM sys_sync_event_trace), (SELECT count(*) "
        "FROM sys_node_release_target WHERE release_id = '00000000-0000-4000-8000-000000000381'), (SELECT count(*) "
        "FROM sys_node_release_target), (SELECT count(*) FROM sys_sync_task WHERE subtask_id IS NULL), (SELECT "
        "count(*) FROM sys_operation), (SELECT count(*) FROM sys_cluster_release)");
    require(integer(counts, 0) == 0 && integer(counts, 1) == 0, "retained attempts and their traces are deleted");
    require(integer(counts, 2) == 0 && integer(counts, 3) == 2, "only targets of superseded releases are deleted");
    require(integer(counts, 4) == 2 && integer(counts, 5) == 0 && integer(counts, 6) == 2,
        "markers and releases stay, unowned; the task center starts empty");
    require(co_await scheduling_fingerprint(db) == upgrade_fingerprint,
        "markers, current targets, releases and resource state are unchanged");
}

// A server without task tracking (rollback) keeps working on the new schema:
// it never names a subtask, and its unowned work becomes visible when it fails.
ruvia::task<void> verify_old_server(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute(
        "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, outcome, "
        "error, attempt) VALUES ($1, '00000000-0000-4000-8000-000000000372', 'dns_zone', $2, 'sync', 5, 'failed', "
        "'old server', 2)",
        tenant, zone(2));
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, registration_status, registered_at, agent_id, "
        "node_secret_hash, node_secret_envelope) VALUES ($1, $2, $3, 'Upgrade C', 'registered', NOW(), repeat('e', "
        "32), repeat('f', 64), 'qa-only')",
        node_spare, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_node_release_target(tenant_id, release_id, node_id, status) VALUES ($1, "
        "'00000000-0000-4000-8000-000000000382', $2, 'pending')",
        tenant, node_spare);
    const auto unowned = co_await db.query(
        "SELECT (SELECT count(*) FROM sys_sync_event WHERE subtask_id IS NULL), (SELECT count(*) FROM "
        "sys_node_release_target WHERE node_id = $1 AND subtask_id IS NULL)",
        node_spare);
    require(integer(unowned, 0) == 1 && integer(unowned, 1) == 1, "old-server inserts work without subtask_id");

    // The new server queues zone 1 for a task; the old server's upsert then
    // moves the marker to a newer version without touching subtask_id.
    auto origin = work_origin::system(action::system_dns_cleanup, "dns_zone", zone(1));
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await service::sync_runtime::upsert_marker(transaction, tenant, marker_resource_type::dns_zone,
            zone(1), marker_operation::sync, 4, origin);
        co_await transaction.commit();
    }
    const auto owned = co_await db.query("SELECT subtask_id::text FROM sys_sync_task WHERE dns_zone_id = $1", zone(1));
    (void)co_await db.query(
        "INSERT INTO sys_sync_task (tenant_id, resource_type, dns_zone_id, operation, version, processed_version, "
        "is_done, is_ok, error, count_fails, next_attempt_at, created_at, updated_at) VALUES ($1, 'dns_zone', $2, "
        "'sync', 6, 0, FALSE, FALSE, '', 0, NOW(), NOW(), NOW()) ON CONFLICT (tenant_id, resource_type, "
        "resource_id) DO UPDATE SET version = GREATEST(sys_sync_task.version, EXCLUDED.version), operation = CASE "
        "WHEN EXCLUDED.version >= sys_sync_task.version THEN EXCLUDED.operation ELSE sys_sync_task.operation END, "
        "is_done = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE sys_sync_task.is_done END, "
        "is_ok = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN FALSE ELSE sys_sync_task.is_ok END, "
        "processed_version = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN 0 ELSE "
        "sys_sync_task.processed_version END, error = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN '' "
        "ELSE sys_sync_task.error END, count_fails = CASE WHEN EXCLUDED.version >= sys_sync_task.version THEN 0 "
        "ELSE sys_sync_task.count_fails END, next_attempt_at = CASE WHEN EXCLUDED.version >= sys_sync_task.version "
        "THEN NOW() ELSE sys_sync_task.next_attempt_at END, updated_at = CASE WHEN EXCLUDED.version >= "
        "sys_sync_task.version THEN NOW() ELSE sys_sync_task.updated_at END, lease_owner = CASE WHEN "
        "EXCLUDED.version > sys_sync_task.version THEN NULL ELSE sys_sync_task.lease_owner END, lease_until = CASE "
        "WHEN EXCLUDED.version > sys_sync_task.version THEN NULL ELSE sys_sync_task.lease_until END RETURNING id",
        tenant, zone(1));
    const auto drifted = co_await db.query(
        "SELECT id::text, subtask_id::text, version FROM sys_sync_task WHERE dns_zone_id = $1", zone(1));
    require(text(drifted, 1) == text(owned) && integer(drifted, 2) == 6,
        "the old upsert keeps the subtask of an older version");
    // Its failure on the new server is adopted by 「后台任务」.
    (void)co_await db.execute(
        "UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds' WHERE id = $1",
        text(drifted), worker);
    const auto marker_id = text(drifted);
    const auto drifted_zone = zone(1);
    const auto lease = service::sync_runtime::make_running_lease(tenant, marker_id, 6, worker,
        std::chrono::steady_clock::now(), marker_resource_type::dns_zone, drifted_zone);
    auto transaction = co_await db.begin_transaction();
    const auto failed = co_await service::sync_runtime::fail_running_and_record_event(transaction, lease,
        "drifted failure", service::sync_runtime::detail_none_json);
    co_await transaction.commit();
    const auto adopted = co_await db.query(
        "SELECT operation.action, subtask.version FROM sys_sync_task marker JOIN sys_operation_subtask subtask ON "
        "subtask.id = marker.subtask_id JOIN sys_operation operation ON operation.id = subtask.operation_id WHERE "
        "marker.id = $1",
        marker_id);
    require(failed.event_recorded_ && !failed.owner_.empty() && text(adopted) == "system.untracked" &&
                integer(adopted, 1) == 6,
        "drifted work belongs to the system task once it fails");
}

ruvia::task<void> break_owner_column(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute("INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'Drift QA', 'drift-qa')", tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, outcome, "
        "error, attempt) VALUES ($1, $2, 'dns_zone', $2, 'sync', 1, 'failed', 'kept', 1)",
        tenant, zone(1));
    (void)co_await db.execute("ALTER TABLE sys_node_release_target ADD COLUMN subtask_id text");
}

ruvia::task<void> verify_drift_rolled_back(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto state = co_await db.query(
        "SELECT (SELECT count(*) FROM schema_migrations WHERE migration_id = '0025_operation_tasks'), (SELECT "
        "count(*) FROM information_schema.tables WHERE table_name = 'sys_operation'), (SELECT count(*) FROM "
        "sys_sync_event), (SELECT count(*) FROM information_schema.columns WHERE table_name = 'sys_sync_task' AND "
        "column_name = 'subtask_id')");
    require(integer(state, 0) == 0 && integer(state, 1) == 0 && integer(state, 2) == 1 && integer(state, 3) == 0,
        "a rejected 0025 leaves the 0024 database untouched, retained attempts included");
}

// ---------------------------------------------------------------------------
// Work fixture

ruvia::task<void> insert_fixture(ruvia::db_handle& db) {
    (void)co_await db.execute(
        "INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'Operation QA', 'operation-qa'), ($2, 'Operation other', "
        "'operation-other')",
        tenant, other_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_admin(id, username, password_hash, nickname) VALUES ($1, 'ops-qa', 'qa-only', '运维小王')", admin);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id, status) VALUES ($1, $3, 'dns', "
        "'cloudflare', 'Operation DNS', 'acct', 'verified'), ($2, $4, 'dns', 'cloudflare', 'Other DNS', 'acct', "
        "'verified')",
        dns_provider, other_provider, tenant, other_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, status) VALUES ($1, $2, 'certificate', "
        "'letsencrypt', 'verified')",
        certificate_provider, tenant);
    // The schema's empty JSON default is not a valid DNS runtime. These
    // synced zones need the same complete runtime shape as production writes.
    service::dns_sync::zone_runtime_data runtime{};
    runtime.records_imported_ = true;
    const auto runtime_json = ruvia::to_json(service::dns_sync::to_output(runtime));
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, sync_status, synced_revision, runtime) VALUES "
        "($1, $2, $3, 'edge.example.com', 'synced', 1, $4::jsonb)",
        cluster_zone, tenant, dns_provider, std::string_view(runtime_json));
    for (int index = 1; index <= 17; ++index) {
        (void)co_await db.execute(
            "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, sync_status, synced_revision, runtime) "
            "VALUES ($1, $2, $3, $4, 'synced', 1, $5::jsonb)",
            zone(index), tenant, dns_provider, zone_domain(index), std::string_view(runtime_json));
    }
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, runtime) VALUES "
        "($1, $2, $3, 'other.example.com', $4::jsonb)",
        other_zone, other_tenant, other_provider, std::string_view(runtime_json));
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(id, tenant_id, dns_zone_id, name, hostname_prefix) VALUES ($1, $5, $6, 'Edge', "
        "'edge'), ($2, $5, $6, 'Failing', 'failing'), ($3, $5, $6, 'Partial', 'partial'), ($4, $5, $6, 'Orphan', "
        "'orphan')",
        cluster, failing_cluster, partial_cluster, orphan_cluster, tenant, cluster_zone);
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, registration_status, registered_at, agent_id, "
        "node_secret_hash, node_secret_envelope, last_heartbeat_at) SELECT fixture.id::uuid, $1, "
        "fixture.cluster_id::uuid, fixture.name, 'registered', NOW(), md5(fixture.name), repeat('a', 64), 'qa-only', "
        "CASE WHEN fixture.online THEN NOW() END FROM (VALUES ($2, $8, '北京', TRUE), ($3, $8, '上海', FALSE), ($4, "
        "$8, '广州', TRUE), ($5, $9, '深圳', TRUE), ($6, $10, '杭州', TRUE), ($7, $11, '成都', TRUE)) fixture(id, "
        "cluster_id, name, online)",
        tenant, node_online, node_offline, node_spare, node_failing, node_partial, node_orphan, cluster,
        failing_cluster, partial_cluster, orphan_cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_certificate(id, tenant_id, provider_id, dns_zone_id, domain) VALUES ($1, $2, $3, $4, "
        "'www.z17.example.com')",
        certificate, tenant, certificate_provider, zone(17));
}

// Queues a DNS sync of zone `index` at `version` for `origin`, committed.
ruvia::task<std::string> queue_zone(ruvia::db_handle& db, int index, std::int64_t version, work_origin& origin) {
    auto transaction = co_await db.begin_transaction();
    (void)co_await origin.require_operation(transaction, tenant);
    const auto marker = co_await service::sync_runtime::upsert_marker(transaction, tenant,
        marker_resource_type::dns_zone, zone(index), marker_operation::sync, version, origin);
    co_await transaction.commit();
    co_return marker;
}

ruvia::task<void> publish(ruvia::db_handle& db, std::string_view cluster_id, work_origin& origin) {
    auto transaction = co_await db.begin_transaction();
    (void)co_await origin.require_operation(transaction, tenant);
    co_await service::node_dispatch::publish_cluster_release(transaction, tenant, cluster_id, origin);
    co_await transaction.commit();
}

ruvia::task<std::string> current_release(ruvia::db_handle& db, std::string_view cluster_id) {
    co_return text(co_await db.query("SELECT current_release_id::text FROM sys_cluster WHERE id = $1", cluster_id));
}

// Runs one attempt of the zone's marker at its current version through the
// production result transition; `recorder` receives the notification.
ruvia::task<service::sync_runtime::running_result_transition> run_zone(ruvia::db_handle& db, int index, bool ok,
    std::string_view detail = service::sync_runtime::detail_none_json,
    test_support::recorded_notifications* recorder = nullptr) {
    const auto zone_id = zone(index);
    const auto marker = co_await db.query(
        "UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds' WHERE tenant_id = $3 "
        "AND dns_zone_id = $1 RETURNING id::text, version",
        zone_id, worker, tenant);
    require(!marker.empty(), "zone " + std::to_string(index) + " has a marker");
    const auto marker_id = text(marker);
    const auto lease = service::sync_runtime::make_running_lease(tenant, marker_id, integer(marker, 1), worker,
        std::chrono::steady_clock::now() - std::chrono::milliseconds(20), marker_resource_type::dns_zone, zone_id);
    auto transaction = co_await db.begin_transaction();
    const auto transition =
        ok ? co_await service::sync_runtime::complete_running_and_record_event(transaction, lease, detail)
           : co_await service::sync_runtime::fail_running_and_record_event(transaction, lease, "QA 同步失败", detail);
    if (recorder != nullptr) {
        service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "dns-sync",
            std::chrono::steady_clock::now());
        co_await service::sync_runtime::commit_and_publish_result_event(transaction, *recorder, lease, transition,
            trace);
    } else {
        co_await transaction.commit();
    }
    co_return transition;
}

ruvia::task<std::string> subtask_of(ruvia::db_handle& db, std::string_view operation, std::string_view resource) {
    co_return text(co_await db.query(
        "SELECT id::text FROM sys_operation_subtask WHERE operation_id = $1::uuid AND resource_id = $2::uuid ORDER BY "
        "created_at DESC LIMIT 1",
        operation, resource));
}

const service::task::subtask_dto& subtask_in(const service::task::operation_detail_dto& detail,
    std::string_view resource, std::string_view kind = {}) {
    for (const auto& subtask : detail.get<"subtasks">()) {
        if (subtask.get<"resource_id">().view() == resource && (kind.empty() || subtask.get<"kind">().view() == kind)) {
            return subtask;
        }
    }
    throw std::runtime_error("operation tasks: no subtask on " + std::string(resource));
}

std::string operation_of(const work_origin& origin) {
    require(origin.operation_id().has_value(), "the write created its task");
    return *origin.operation_id();
}

// ---------------------------------------------------------------------------
// Merge and cancel

struct merge_results final {
    std::string merged_operation_;
    std::string cancelled_operation_;
};

ruvia::task<merge_results> verify_merge_and_cancel(ruvia::db_handle& db, read_context& context) {
    // Two quick saves: the first one's pending sync is merged into the second.
    auto first = user(action::dns_zone_update_records, "dns_zone", zone(1), R"({"record_counts":{"created":1}})");
    (void)co_await queue_zone(db, 1, 2, first);
    auto second = user(action::dns_zone_update_records, "dns_zone", zone(1));
    (void)co_await queue_zone(db, 1, 3, second);
    const auto first_id = operation_of(first);
    const auto second_id = operation_of(second);
    require(first_id != second_id, "consecutive user writes always have distinct root tasks");
    require(std::find(second.touched().begin(), second.touched().end(), first_id) != second.touched().end(),
        "the merged task is notified with the write");
    const auto merged = co_await tasks().detail(context, std::string(tenant), first_id);
    const auto& merged_subtask = subtask_in(merged, zone(1));
    require(merged.get<"status">().view() == "merged" && merged_subtask.get<"status">().view() == "merged" &&
                merged_subtask.get<"merged_into">().has_value() &&
                merged_subtask.get<"merged_into">()->get<"operation_id">().view() == second_id &&
                merged_subtask.get<"merged_into">()->get<"subtask_id">().view() ==
                    co_await subtask_of(db, second_id, zone(1)) &&
                merged_subtask.get<"merged_into">()->get<"action">().view() == "dns_zone.update_records" &&
                merged_subtask.get<"merged_into">()->get<"target_name">().view() == zone_domain(1) &&
                merged_subtask.get<"merged_into">()->get<"status">().view() == "queued" &&
                !merged_subtask.get<"closed_by">().has_value() && !merged.get<"finished_at">().view().empty(),
        "first save merged into the second, which shows its live status");

    // Work that already ran keeps its result when a newer save arrives.
    (void)co_await run_zone(db, 1, true);
    auto third = user(action::dns_zone_sync, "dns_zone", zone(1));
    (void)co_await queue_zone(db, 1, 4, third);
    const auto ran = co_await tasks().detail(context, std::string(tenant), second_id);
    require(ran.get<"status">().view() == "completed" &&
                subtask_in(ran, zone(1)).get<"status">().view() == "completed" &&
                subtask_in(ran, zone(1)).get<"attempt_count">().value_ == 1 &&
                text(co_await db.query("SELECT state FROM sys_operation_subtask WHERE id = $1::uuid",
                    co_await subtask_of(db, second_id, zone(1)))) == "completed",
        "completed work settles as completed, not merged");

    // An older version merges into the current owner.
    auto late = user(action::dns_zone_sync, "dns_zone", zone(1));
    (void)co_await queue_zone(db, 1, 3, late);
    const auto stale = co_await tasks().detail(context, std::string(tenant), operation_of(late));
    require(subtask_in(stale, zone(1)).get<"status">().view() == "merged" &&
                subtask_in(stale, zone(1)).get<"merged_into">()->get<"operation_id">().view() == operation_of(third) &&
                integer(co_await db.query("SELECT version FROM sys_sync_task WHERE dns_zone_id = $1", zone(1))) == 4,
        "an older version never replaces the current owner");

    // Merge pointers follow the work: a merged subtask points at the newest owner.
    auto chain_a = user(action::dns_zone_sync, "dns_zone", zone(2));
    auto chain_b = user(action::dns_zone_sync, "dns_zone", zone(2));
    auto chain_c = user(action::dns_zone_sync, "dns_zone", zone(2));
    (void)co_await queue_zone(db, 2, 2, chain_a);
    (void)co_await queue_zone(db, 2, 3, chain_b);
    (void)co_await queue_zone(db, 2, 4, chain_c);
    const auto pointer = co_await db.query("SELECT merged_into_id::text FROM sys_operation_subtask WHERE id = $1::uuid",
        co_await subtask_of(db, operation_of(chain_a), zone(2)));
    require(text(pointer) == co_await subtask_of(db, operation_of(chain_c), zone(2)),
        "subtasks merged earlier follow to the newest owner");

    // One write dirtying a zone twice has one subtask at the newer version.
    auto twice = user(action::cluster_update, "cluster", cluster);
    (void)co_await queue_zone(db, 3, 2, twice);
    (void)co_await queue_zone(db, 3, 3, twice);
    const auto single = co_await db.query(
        "SELECT count(*), max(version) FROM sys_operation_subtask WHERE operation_id = $1::uuid", operation_of(twice));
    require(integer(single, 0) == 1 && integer(single, 1) == 3, "one subtask per resource and task");

    // Node publish: a release supersedes pending targets of the previous one.
    auto save_a = user(action::website_update, "cluster", cluster);
    co_await publish(db, cluster, save_a);
    const auto release_a = co_await current_release(db, cluster);
    const auto published = co_await tasks().detail(context, std::string(tenant), operation_of(save_a));
    require(published.get<"subtasks">().size() == 3 && published.get<"status">().view() == "queued" &&
                subtask_in(published, node_offline).get<"node_offline">().value_ &&
                !subtask_in(published, node_online).get<"node_offline">().value_ &&
                subtask_in(published, node_online).get<"resource_name">().view() == "北京",
        "one node publish subtask per registered node; offline only for the offline node");
    {
        auto transaction = co_await db.begin_transaction();
        require(co_await service::node_dispatch::mark_node_target_applied(transaction, tenant, release_a, node_online),
            "target applied");
        co_await transaction.commit();
    }
    auto save_b = user(action::website_update, "cluster", cluster);
    co_await publish(db, cluster, save_b);
    const auto superseded = co_await tasks().detail(context, std::string(tenant), operation_of(save_a));
    require(superseded.get<"status">().view() == "completed" &&
                subtask_in(superseded, node_online).get<"status">().view() == "completed" &&
                !subtask_in(superseded, node_online).get<"node_offline">().value_ &&
                subtask_in(superseded, node_offline).get<"status">().view() == "merged" &&
                !subtask_in(superseded, node_offline).get<"node_offline">().value_ &&
                subtask_in(superseded, node_offline).get<"merged_into">()->get<"operation_id">().view() ==
                    operation_of(save_b),
        "applied targets stay completed, pending ones merge into the same node of the new release");

    // A node that is no longer a target is cancelled by the new release.
    (void)co_await db.execute("UPDATE sys_node SET status = 'disabled' WHERE id = $1", node_spare);
    auto save_c = user(action::cluster_update, "cluster", cluster);
    co_await publish(db, cluster, save_c);
    const auto mixed = co_await tasks().detail(context, std::string(tenant), operation_of(save_b));
    require(subtask_in(mixed, node_spare).get<"status">().view() == "cancelled" &&
                subtask_in(mixed, node_spare).get<"closed_by">()->get<"operation_id">().view() ==
                    operation_of(save_c) &&
                subtask_in(mixed, node_spare).get<"closed_by">()->get<"action">().view() == "cluster.update" &&
                mixed.get<"status">().view() == "cancelled",
        "merged and cancelled subtasks roll up as cancelled");

    // Deleting a node is a task even when it only cancels someone else's work.
    auto offline_delete = user(action::node_delete, "node", node_offline);
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await offline_delete.require_operation(transaction, tenant);
        co_await service::node_dispatch::exclude_pending_node_targets(transaction, std::string(tenant),
            std::string(node_offline), offline_delete);
        co_await transaction.commit();
    }
    const auto deleted = co_await tasks().detail(context, std::string(tenant), operation_of(offline_delete));
    require(deleted.get<"action">().view() == "node.delete" && deleted.get<"status">().view() == "completed" &&
                deleted.get<"subtasks">().empty() && offline_delete.touched().size() == 2,
        "cancelling alone creates a completed root and notifies both operations");
    auto node_delete = user(action::node_delete, "node", node_online);
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await node_delete.require_operation(transaction, tenant);
        co_await service::cluster_dns::reconcile_cluster(transaction, std::string(tenant), std::string(cluster),
            node_delete);
        co_await service::node_dispatch::exclude_pending_node_targets(transaction, std::string(tenant),
            std::string(node_online), node_delete);
        co_await transaction.commit();
    }
    const auto cancelled = co_await tasks().detail(context, std::string(tenant), operation_of(save_c));
    require(cancelled.get<"status">().view() == "cancelled" &&
                subtask_in(cancelled, node_offline).get<"status">().view() == "cancelled" &&
                subtask_in(cancelled, node_offline).get<"closed_by">().has_value() &&
                subtask_in(cancelled, node_offline).get<"closed_by">()->get<"operation_id">().view() ==
                    operation_of(offline_delete) &&
                !subtask_in(cancelled, node_offline).get<"node_offline">().value_ &&
                subtask_in(cancelled, node_online).get<"closed_by">()->get<"operation_id">().view() ==
                    operation_of(node_delete) &&
                subtask_in(cancelled, node_online).get<"closed_by">()->get<"target_name">().view() == "北京",
        "excluded targets name the deleting task even when that task has no background work");

    // Node configuration: a newer spec merges the older one; reset cancels.
    auto config_a = user(action::node_update, "node", node_spare, R"({"sections":["basic"]})");
    auto config_b = user(action::node_update, "node", node_spare, R"({"sections":["endpoints"]})");
    auto reset = user(action::node_reset_credentials, "node", node_spare);
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await config_a.require_operation(transaction, tenant);
        co_await service::node_dispatch::track_node_config(transaction, tenant, node_spare, 2, config_a);
        co_await transaction.commit();
    }
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await config_b.require_operation(transaction, tenant);
        co_await service::node_dispatch::track_node_config(transaction, tenant, node_spare, 3, config_b);
        co_await transaction.commit();
    }
    const auto config_merged = co_await tasks().detail(context, std::string(tenant), operation_of(config_a));
    require(subtask_in(config_merged, node_spare, "node_config").get<"status">().view() == "merged" &&
                subtask_in(config_merged, node_spare)
                        .get<"merged_into">()
                        ->get<"subtask_id">()
                        .view() == co_await subtask_of(db, operation_of(config_b), node_spare) &&
                config_merged.get<"summary">().view().find("basic") != std::string_view::npos,
        "node configuration merges into the newer spec revision");
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await reset.require_operation(transaction, tenant);
        co_await service::node_dispatch::cancel_node_config(transaction, tenant, node_spare, reset);
        co_await transaction.commit();
    }
    const auto config_cancelled = co_await tasks().detail(context, std::string(tenant), operation_of(config_b));
    require(config_cancelled.get<"status">().view() == "cancelled" &&
                subtask_in(config_cancelled, node_spare, "node_config")
                        .get<"closed_by">()
                        ->get<"operation_id">()
                        .view() == operation_of(reset),
        "reset credentials cancels pending configuration and owns the cancellation");
    co_return merge_results{.merged_operation_ = first_id, .cancelled_operation_ = operation_of(save_c)};
}

// ---------------------------------------------------------------------------
// Status derivation and roll-up (contract 5.2)

struct status_results final {
    std::string retrying_;
    std::string running_;
    std::string queued_;
    std::string attention_;
    std::string failed_;
    std::string partial_;
    std::string empty_;
};

ruvia::task<status_results> verify_statuses(ruvia::db_handle& db, read_context& context) {
    status_results result;
    const auto lease_zone = [&](int index) -> ruvia::task<void> {
        (void)co_await db.execute(
            "UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds', updated_at = "
            "NOW() WHERE dns_zone_id = $1",
            zone(index), worker);
    };
    const auto fail_zone = [&](int index) -> ruvia::task<void> {
        (void)co_await db.execute(
            "UPDATE sys_sync_task SET count_fails = 1, error = 'QA 重试', next_attempt_at = NOW() + INTERVAL '15 "
            "seconds' WHERE dns_zone_id = $1",
            zone(index));
    };

    // retrying > running > queued.
    auto retrying = user(action::cluster_update, "cluster", cluster);
    for (const int index : {4, 5, 6}) {
        (void)co_await queue_zone(db, index, 2, retrying);
    }
    co_await lease_zone(5);
    co_await fail_zone(6);
    result.retrying_ = operation_of(retrying);
    const auto retry = co_await tasks().detail(context, std::string(tenant), result.retrying_);
    require(retry.get<"status">().view() == "retrying" && subtask_in(retry, zone(4)).get<"status">().view() == "queued" &&
                subtask_in(retry, zone(5)).get<"status">().view() == "running" &&
                !subtask_in(retry, zone(5)).get<"running_since">().view().empty() &&
                subtask_in(retry, zone(6)).get<"status">().view() == "retrying" &&
                subtask_in(retry, zone(6)).get<"error">().view() == "QA 重试" &&
                !subtask_in(retry, zone(6)).get<"next_attempt_at">().view().empty() &&
                retry.get<"failed_count">().value_ == 1 && retry.get<"settled_count">().value_ == 0 &&
                retry.get<"subtask_count">().value_ == 3 && retry.get<"error">().view() == "QA 重试" &&
                retry.get<"next_attempt_at">().view() == subtask_in(retry, zone(6)).get<"next_attempt_at">().view() &&
                retry.get<"finished_at">().view().empty() && retry.get<"actor_type">().view() == "user" &&
                retry.get<"actor_name">().view() == "运维小王" && retry.get<"target_name">().view() == "Edge",
        "retrying wins the roll-up; each subtask follows its marker");

    auto running = user(action::cluster_update, "cluster", cluster);
    (void)co_await queue_zone(db, 7, 2, running);
    (void)co_await queue_zone(db, 8, 2, running);
    co_await lease_zone(7);
    result.running_ = operation_of(running);
    const auto leased = co_await tasks().detail(context, std::string(tenant), result.running_);
    require(leased.get<"status">().view() == "running", "running before queued");

    auto queued = user(action::cluster_update, "cluster", cluster);
    (void)co_await queue_zone(db, 9, 2, queued);
    (void)co_await queue_zone(db, 10, 2, queued);
    (void)co_await run_zone(db, 10, true);
    result.queued_ = operation_of(queued);
    const auto waiting = co_await tasks().detail(context, std::string(tenant), result.queued_);
    require(waiting.get<"status">().view() == "queued" && waiting.get<"settled_count">().value_ == 1 &&
                subtask_in(waiting, zone(10)).get<"status">().view() == "completed" &&
                !subtask_in(waiting, zone(10)).get<"finished_at">().view().empty(),
        "queued while any subtask waits");

    // A completed sync that left conflicts needs attention.
    auto attention = work_origin::system(action::system_dns_cleanup, "dns_zone", zone(11));
    (void)co_await queue_zone(db, 11, 2, attention);
    (void)co_await run_zone(db, 11, false);
    (void)co_await run_zone(db, 11, true,
        R"({"kind":"dns_sync","truncated":false,"created":[],"updated":[],"deleted":[],"created_count":0,"updated_count":0,"deleted_count":0,"unchanged_count":0,"conflicts":[],"conflict_count":1})");
    result.attention_ = operation_of(attention);
    const auto warned = co_await tasks().detail(context, std::string(tenant), result.attention_);
    require(warned.get<"status">().view() == "attention_required" &&
                subtask_in(warned, zone(11)).get<"warning">().view() == "dns_conflict" &&
                subtask_in(warned, zone(11)).get<"recovered">().value_ &&
                subtask_in(warned, zone(11)).get<"failure_count">().value_ == 1 &&
                warned.get<"actor_type">().view() == "system" && warned.get<"actor_name">().view().empty(),
        "a completed sync with conflicts is attention_required");

    // Node failures that cannot be retried: failed, or partial_failed beside
    // completed work.
    auto failing = user(action::cluster_update, "cluster", failing_cluster);
    co_await publish(db, failing_cluster, failing);
    // One connection per client: read the release before opening the transaction.
    const auto failing_release = co_await current_release(db, failing_cluster);
    {
        auto transaction = co_await db.begin_transaction();
        require(co_await service::node_dispatch::mark_node_target_failed(transaction, tenant, failing_release,
                    node_failing, failing_cluster,
                    {.phase_ = "validate", .error_code_ = "artifact_invalid", .error_ = "配置校验失败", .retryable_ = true}),
            "retryable failure");
        co_await transaction.commit();
    }
    result.failed_ = operation_of(failing);
    const auto retried = co_await tasks().detail(context, std::string(tenant), result.failed_);
    require(retried.get<"status">().view() == "retrying", "a retryable node failure is retrying");
    {
        auto transaction = co_await db.begin_transaction();
        require(co_await service::node_dispatch::mark_node_target_failed(transaction, tenant, failing_release,
                    node_failing, failing_cluster,
                    {.phase_ = "validate", .error_code_ = "artifact_invalid", .error_ = "配置校验失败", .retryable_ = false}),
            "final failure");
        co_await transaction.commit();
    }
    const auto failed = co_await tasks().detail(context, std::string(tenant), result.failed_);
    require(failed.get<"status">().view() == "failed" && failed.get<"error">().view() == "配置校验失败" &&
                failed.get<"failed_count">().value_ == 1 && !failed.get<"finished_at">().view().empty() &&
                subtask_in(failed, node_failing).get<"status">().view() == "failed" &&
                !subtask_in(failed, node_failing).get<"node_offline">().value_,
        "a node failure that cannot be retried is failed");

    auto partial = user(action::cluster_update, "cluster", partial_cluster);
    co_await publish(db, partial_cluster, partial);
    (void)co_await queue_zone(db, 12, 2, partial);
    (void)co_await run_zone(db, 12, true);
    const auto partial_release = co_await current_release(db, partial_cluster);
    {
        auto transaction = co_await db.begin_transaction();
        require(co_await service::node_dispatch::mark_node_target_failed(transaction, tenant, partial_release,
                    node_partial, partial_cluster,
                    {.phase_ = "activate", .error_code_ = "disk", .error_ = "磁盘已满", .retryable_ = false}),
            "partial failure");
        co_await transaction.commit();
    }
    result.partial_ = operation_of(partial);
    const auto mixed = co_await tasks().detail(context, std::string(tenant), result.partial_);
    require(mixed.get<"status">().view() == "partial_failed" && mixed.get<"settled_count">().value_ == 2,
        "failed beside completed work is partial_failed");

    // A task without subtasks is completed.
    auto empty = user(action::provider_update, "provider", dns_provider, R"({"credentials_changed":false})");
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await empty.require_operation(transaction, tenant);
        co_await transaction.commit();
    }
    result.empty_ = operation_of(empty);
    const auto nothing = co_await tasks().detail(context, std::string(tenant), result.empty_);
    require(nothing.get<"status">().view() == "completed" && nothing.get<"subtask_count">().value_ == 0 &&
                nothing.get<"finished_at">().view() == nothing.get<"created_at">().view() &&
                nothing.get<"target_kind">().has_value() && nothing.get<"target_kind">()->view() == "dns" &&
                nothing.get<"target_name">().view() == "Operation DNS",
        "a task without subtasks is completed; providers name their kind");
    co_return result;
}

ruvia::task<void> verify_list(read_context& context, const status_results& statuses, const merge_results& merges) {
    const auto list = [&](service::task::list_filter filter,
                          std::int64_t page_size = 100) -> ruvia::task<service::task::operation_page_dto> {
        co_return co_await tasks().list(context, std::string(tenant), 1, page_size, 0, filter);
    };
    const auto ids = [](const service::task::operation_page_dto& page) {
        std::vector<std::string> values;
        for (const auto& item : page.get<"list">()) {
            values.emplace_back(item.get<"id">().view());
        }
        return values;
    };
    const auto contains = [](const std::vector<std::string>& values, std::string_view id) {
        return std::find(values.begin(), values.end(), id) != values.end();
    };
    const auto all = co_await list({});
    const auto everything = ids(all);
    require(all.get<"total">().value_ == static_cast<std::int64_t>(everything.size()) && everything.size() > 10,
        "the list holds every task of the tenant");
    std::int64_t active = 0;
    std::int64_t attention = 0;
    for (const auto& item : all.get<"list">()) {
        const auto status = item.get<"status">().view();
        active += status == "queued" || status == "running" || status == "retrying" ? 1 : 0;
        attention +=
            status == "retrying" || status == "failed" || status == "partial_failed" || status == "attention_required"
                ? 1
                : 0;
    }
    require(all.get<"active">().value_ == active && all.get<"attention">().value_ == attention,
        "active and attention counts cover the tenant");
    for (std::size_t index = 1; index < all.get<"list">().size(); ++index) {
        require(all.get<"list">()[index - 1].get<"created_at">().view() >=
                    all.get<"list">()[index].get<"created_at">().view(),
            "newest first");
    }

    const auto status_filter = [](std::string status) {
        return service::task::list_filter{.status_ = std::move(status),
            .action_ = {},
            .target_type_ = {},
            .actor_ = {},
            .admin_id_ = {},
            .days_ = 0,
            .keyword_ = {}};
    };
    const auto attention_ids = ids(co_await list(status_filter("attention")));
    require(contains(attention_ids, statuses.retrying_) && contains(attention_ids, statuses.failed_) &&
                contains(attention_ids, statuses.partial_) && contains(attention_ids, statuses.attention_) &&
                !contains(attention_ids, statuses.running_) && !contains(attention_ids, statuses.empty_),
        "attention group");
    const auto active_ids = ids(co_await list(status_filter("active")));
    require(contains(active_ids, statuses.retrying_) && contains(active_ids, statuses.running_) &&
                contains(active_ids, statuses.queued_) && !contains(active_ids, statuses.failed_),
        "active group");
    const auto merged_ids = ids(co_await list(status_filter("merged")));
    const auto cancelled_ids = ids(co_await list(status_filter("cancelled")));
    require(contains(merged_ids, merges.merged_operation_) && contains(cancelled_ids, merges.cancelled_operation_),
        "single status filters");
    const auto mine = ids(co_await list({.status_ = {},
        .action_ = {},
        .target_type_ = {},
        .actor_ = "me",
        .admin_id_ = std::string(admin),
        .days_ = 0,
        .keyword_ = {}}));
    const auto system = ids(co_await list({.status_ = {},
        .action_ = {},
        .target_type_ = {},
        .actor_ = "system",
        .admin_id_ = {},
        .days_ = 0,
        .keyword_ = {}}));
    require(contains(mine, statuses.retrying_) && !contains(mine, statuses.attention_) &&
                contains(system, statuses.attention_) && !contains(system, statuses.retrying_) &&
                mine.size() + system.size() == everything.size(),
        "actor filter");
    const auto action_filter = [](std::string code) {
        return service::task::list_filter{.status_ = {},
            .action_ = std::move(code),
            .target_type_ = {},
            .actor_ = {},
            .admin_id_ = {},
            .days_ = 0,
            .keyword_ = {}};
    };
    const auto by_action = co_await list(action_filter("provider.update"));
    require(by_action.get<"total">().value_ == 1 && by_action.get<"list">()[0].get<"id">().view() == statuses.empty_,
        "action filter");
    const auto unknown_action = co_await list(action_filter("unknown.action"));
    require(unknown_action.get<"total">().value_ == 0,
        "an unknown action lists nothing");
    const auto providers = ids(co_await list({.status_ = {},
        .action_ = {},
        .target_type_ = "provider",
        .actor_ = {},
        .admin_id_ = {},
        .days_ = 0,
        .keyword_ = {}}));
    require(providers.size() == 1, "target type filter");
    const auto keyword = ids(co_await list({.status_ = {}, .action_ = {}, .target_type_ = {}, .actor_ = {}, .admin_id_ = {}, .days_ = 1, .keyword_ = "Z11.EXAMPLE"}));
    require(keyword.size() == 1 && keyword[0] == statuses.attention_, "keyword matches the target name");
    const auto paged = co_await list({}, 3);
    require(paged.get<"list">().size() == 3 && paged.get<"total_pages">().value_ == (all.get<"total">().value_ + 2) / 3,
        "pagination");
}

// ---------------------------------------------------------------------------
// Unowned work, live detail pieces, tenant isolation

ruvia::task<work_owner> verify_unowned(ruvia::db_handle& db, read_context& context) {
    // Markers queued before the upgrade (or by an older server) belong to no task.
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(tenant_id, resource_type, dns_zone_id, operation, version) VALUES ($1, 'dns_zone', "
        "$2, 'sync', 1), ($1, 'dns_zone', $3, 'sync', 1)",
        tenant, zone(13), zone(14));
    const auto before = integer(co_await db.query("SELECT count(*) FROM sys_operation"));
    const auto success = co_await run_zone(db, 14, true);
    require(success.event_recorded_ && success.owner_.empty() &&
                integer(co_await db.query("SELECT count(*) FROM sys_operation")) == before,
        "successful unowned work stays hidden");

    test_support::recorded_notifications recorder{.handle_ = db};
    const auto failure = co_await run_zone(db, 13, false, service::sync_runtime::detail_none_json, &recorder);
    require(!failure.owner_.empty(), "a failure of unowned work is adopted");
    const auto adopted = co_await tasks().detail(context, std::string(tenant), failure.owner_.operation_id_);
    require(adopted.get<"action">().view() == "system.untracked" && adopted.get<"actor_type">().view() == "system" &&
                adopted.get<"target_type">().view() == "dns_zone" &&
                adopted.get<"target_name">().view() == zone_domain(13) && adopted.get<"status">().view() == "retrying" &&
                adopted.get<"subtasks">().size() == 1 &&
                subtask_in(adopted, zone(13)).get<"kind">().view() == "dns_sync" &&
                subtask_in(adopted, zone(13)).get<"attempt_count">().value_ == 1,
        "「后台任务」 owns the failed marker and its attempt");

    // The result notification, sent after the commit, names task, subtask
    // and attempt, and survives the stream encoding.
    require(recorder.sent_.size() == 1 && recorder.committed_[0], "one notification after the commit");
    const auto& sent = recorder.sent_[0];
    require(sent.task_attempts_.size() == 1 && sent.task_attempts_[0].task_id_ == failure.owner_.operation_id_ &&
                sent.task_attempts_[0].notice_.subtask_id_ == failure.owner_.subtask_id_ &&
                sent.task_attempts_[0].notice_.event_id_ == *failure.event_id_,
        "attempt notice: operation_id, subtask_id, event_id");
    const auto fields = service::live_resource::notifications::encode(sent);
    const auto decoded = service::live_resource::notifications::decode(fields);
    require(decoded && decoded->tenant_id_ == tenant && decoded->changes_ == sent.changes_ &&
                decoded->task_attempts_.size() == 1 &&
                decoded->task_attempts_[0].task_id_ == failure.owner_.operation_id_ &&
                decoded->task_attempts_[0].notice_.subtask_id_ == failure.owner_.subtask_id_ &&
                decoded->task_attempts_[0].notice_.event_id_ == *failure.event_id_,
        "the attempt notice round-trips through the stream encoding");

    // The open detail of that subtask reads exactly the new attempt.
    const auto events = co_await tasks().recorded_attempts(context, std::string(tenant), failure.owner_.subtask_id_,
        std::span(&sent.task_attempts_[0].notice_, 1));
    const auto event = events.empty() ? std::nullopt
                                      : ruvia::from_json<service::task::subtask_attempt_event_dto>(events[0]);
    require(event && event->get<"subtask_id">().view() == failure.owner_.subtask_id_ &&
                event->get<"item">().get<"attempt">().value_ == 1 &&
                event->get<"item">().get<"trace">().view().find(std::to_string(*failure.event_id_)) != std::string::npos,
        "the detail stream reads the attempt of its expanded subtask");
    const auto other_subtask = co_await db.query(
        "SELECT id::text FROM sys_operation_subtask WHERE id <> $1::uuid LIMIT 1",
        std::string_view(failure.owner_.subtask_id_));
    const auto foreign = co_await tasks().recorded_attempts(context, std::string(tenant), text(other_subtask),
        std::span(&sent.task_attempts_[0].notice_, 1));
    require(!text(other_subtask).empty() && foreign.empty(), "notices of other subtasks read nothing");

    // Later attempts belong to the same subtask; success completes it.
    (void)co_await run_zone(db, 13, false);
    (void)co_await run_zone(db, 13, true);
    const auto recovered = co_await tasks().detail(context, std::string(tenant), failure.owner_.operation_id_);
    const auto history = co_await tasks().history(context, std::string(tenant), failure.owner_.operation_id_,
        failure.owner_.subtask_id_);
    require(recovered.get<"status">().view() == "completed" && subtask_in(recovered, zone(13)).get<"recovered">().value_ &&
                history.get<"list">().size() == 3 && history.get<"list">()[0].get<"attempt">().value_ == 3 &&
                history.get<"summary">().get<"failure_count">().value_ == 2,
        "retries and the final success stay with the adopted subtask");

    // A failed apply of an unowned release target (a release published
    // without task tracking) is adopted likewise.
    auto unowned_release = work_origin::repair();
    co_await publish(db, orphan_cluster, unowned_release);
    const auto release = co_await current_release(db, orphan_cluster);
    require(integer(co_await db.query(
                "SELECT count(*) FROM sys_node_release_target WHERE release_id = $1::uuid AND subtask_id IS NULL",
                release)) == 1,
        "a release without a task has unowned targets");
    auto transaction = co_await db.begin_transaction();
    require(co_await service::node_dispatch::mark_node_target_failed(transaction, tenant, release, node_orphan,
                orphan_cluster, {.phase_ = "stage", .error_code_ = "io", .error_ = "写入失败", .retryable_ = true}),
        "unowned target failed");
    std::pmr::monotonic_buffer_resource pool;
    const auto node_attempt = co_await service::node_dispatch::record_node_apply_attempt(transaction,
        {.tenant_id_ = tenant,
            .release_id_ = release,
            .node_id_ = node_orphan,
            .source_ = "apply_result",
            .applied_ = false,
            .phase_ = "stage",
            .error_code_ = "io",
            .error_ = "写入失败",
            .retryable_ = true,
            .elapsed_milliseconds_ = 10},
        &pool);
    co_await transaction.commit();
    require(node_attempt && !node_attempt->owner_.empty(), "unowned node failure adopted");
    const auto node_task = co_await tasks().detail(context, std::string(tenant), node_attempt->owner_.operation_id_);
    require(node_task.get<"action">().view() == "system.untracked" && node_task.get<"target_name">().view() == "成都" &&
                subtask_in(node_task, node_orphan).get<"kind">().view() == "node_publish" &&
                subtask_in(node_task, node_orphan).get<"status">().view() == "retrying" &&
                subtask_in(node_task, node_orphan).get<"attempt_count">().value_ == 1,
        "「后台任务 · 节点」 owns the failed target and its attempt");
    co_return failure.owner_;
}

ruvia::task<void> verify_tenant_isolation(ruvia::db_handle& db, read_context& context, const work_owner& owned) {
    auto foreign = work_origin::system(action::system_dns_cleanup, "dns_zone", std::string(other_zone));
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await service::sync_runtime::upsert_marker(transaction, other_tenant, marker_resource_type::dns_zone,
            other_zone, marker_operation::sync, 2, foreign);
        co_await transaction.commit();
    }
    const auto page = co_await tasks().list(context, std::string(other_tenant), 1, 100, 0, {});
    require(page.get<"total">().value_ == 1 && page.get<"list">()[0].get<"id">().view() == operation_of(foreign) &&
                page.get<"active">().value_ == 1 && page.get<"attention">().value_ == 0,
        "lists and counts are tenant scoped");
    const auto rejected = [&](auto read) -> ruvia::task<bool> {
        try {
            co_await read();
        } catch (const std::exception&) {
            co_return true;
        }
        co_return false;
    };
    const auto event_id = integer(co_await db.query(
        "SELECT max(id) FROM sys_sync_event WHERE subtask_id = $1::uuid", std::string_view(owned.subtask_id_)));
    require(co_await rejected([&]() -> ruvia::task<void> {
        (void)co_await tasks().detail(context, std::string(other_tenant), owned.operation_id_);
    }),
        "detail is tenant scoped");
    require(co_await rejected([&]() -> ruvia::task<void> {
        (void)co_await tasks().history(context, std::string(other_tenant), owned.operation_id_, owned.subtask_id_);
    }),
        "history is tenant scoped");
    require(co_await rejected([&]() -> ruvia::task<void> {
        (void)co_await tasks().attempt_trace(context, std::string(other_tenant), owned.operation_id_, event_id);
    }),
        "trace is tenant scoped");
    require(co_await rejected([&]() -> ruvia::task<void> {
        (void)co_await tasks().detail(context, std::string(tenant), owned.operation_id_, operation_of(foreign));
    }),
        "a subtask of another task is rejected");
    require(co_await rejected([&]() -> ruvia::task<void> {
        (void)co_await tasks().attempt_trace(context, std::string(tenant), operation_of(foreign), event_id);
    }),
        "an attempt of another task is rejected");
    require((co_await tasks().recorded_attempts(context, std::string(other_tenant), owned.subtask_id_,
                 std::vector<service::live_resource::task_attempt_notice>{
                     {.subtask_id_ = owned.subtask_id_, .event_id_ = event_id}}))
                .empty(),
        "attempt events are tenant scoped");
}

// ---------------------------------------------------------------------------
// Certificate issuance: DNS-01 records are children of the issuance subtask.

ruvia::task<void> verify_certificate_children(ruvia::db_handle& db, read_context& context) {
    auto request = user(action::certificate_create, "certificate", certificate);
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await request.require_operation(transaction, tenant);
        (void)co_await service::certificate_issuance::enqueue_certificate_revision(transaction, tenant, certificate,
            1, marker_operation::issue, request);
        co_await transaction.commit();
    }
    const auto operation = operation_of(request);
    const auto root_count = integer(co_await db.query("SELECT count(*) FROM sys_operation"));
    const auto claimed = co_await db.query(
        "UPDATE sys_sync_task marker SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds' WHERE "
        "certificate_id = $1 RETURNING marker.id::text, " +
            service::sync_runtime::claimed_owner_columns("marker"),
        certificate, worker);
    const auto owner = service::sync_runtime::claimed_owner(claimed.front(), 1);
    require(owner.operation_id_ == operation, "the claim returns the issuance subtask and its task");
    const auto marker_id = text(claimed);
    const auto lease = service::sync_runtime::make_running_lease(tenant, marker_id, 1, worker,
        std::chrono::steady_clock::now(), marker_resource_type::certificate, certificate);
    const auto challenge_zone = zone(17);
    const service::certificate_issuance::dns_challenge_config_view config{.dns_zone_id_ = challenge_zone,
        .zone_domain_ = "z17.example.com",
        .certificate_id_ = certificate,
        .minimum_record_ttl_ = 60,
        .owner_ = owner};
    test_support::recorded_notifications worker_context{.handle_ = db};
    const std::vector<service::certificate_issuance::dns_challenge_spec> specs{
        {.name_ = "_acme-challenge.www", .content_ = "qa-token"}};
    const auto challenge =
        co_await service::certificate_issuance::persist_dns_challenges(worker_context, lease, config, specs);
    require(!challenge.subtask_id_.empty() && worker_context.sent_.size() == 1 &&
                worker_context.sent_[0].changes_.size() == 1 &&
                worker_context.sent_[0].changes_[0].id_ == operation,
        "publishing the challenge notifies the issuance task");
    (void)co_await run_zone(db, 17, true);
    const auto cleanup = co_await service::certificate_issuance::retire_dns_challenges(worker_context, config, lease);
    require(!cleanup.subtask_id_.empty() && cleanup.subtask_id_ != challenge.subtask_id_,
        "cleanup is a new subtask, not a continuation of the completed challenge");
    const auto detail = co_await tasks().detail(context, std::string(tenant), operation);
    const auto& issuance = subtask_in(detail, certificate, "certificate_issue");
    const auto& published = subtask_in(detail, zone(17), "dns_challenge");
    const auto& retired = subtask_in(detail, zone(17), "dns_challenge_cleanup");
    require(detail.get<"subtasks">().size() == 3 && issuance.get<"parent_id">().view().empty() &&
                issuance.get<"id">().view() == owner.subtask_id_ &&
                published.get<"parent_id">().view() == owner.subtask_id_ &&
                retired.get<"parent_id">().view() == owner.subtask_id_ &&
                published.get<"id">().view() == challenge.subtask_id_ &&
                retired.get<"id">().view() == cleanup.subtask_id_ &&
                issuance.get<"status">().view() == "running" && published.get<"status">().view() == "completed" &&
                retired.get<"status">().view() == "queued" && detail.get<"status">().view() == "running" &&
                detail.get<"target_name">().view() == "www.z17.example.com" &&
                detail.get<"subtasks">()[0].get<"id">().view() == owner.subtask_id_ &&
                integer(co_await db.query("SELECT count(*) FROM sys_operation")) == root_count,
        "DNS-01 publish and cleanup remain children of the original user operation, without extra roots");
}

// ---------------------------------------------------------------------------
// Retention (7 days after every subtask settled; unsettled tasks stay)

ruvia::task<void> verify_retention(ruvia::db_handle& db, const status_results& statuses) {
    auto settled = user(action::dns_zone_delete, "dns_zone", zone(15));
    (void)co_await queue_zone(db, 15, 2, settled);
    {
        auto remover = work_origin::repair();
        auto transaction = co_await db.begin_transaction();
        co_await service::sync_runtime::remove_marker(transaction, tenant, marker_resource_type::dns_zone, zone(15),
            remover);
        co_await transaction.commit();
    }
    auto unsettled = user(action::dns_zone_sync, "dns_zone", zone(16));
    (void)co_await queue_zone(db, 16, 2, unsettled);
    for (const auto& id : {operation_of(settled), operation_of(unsettled), statuses.attention_}) {
        (void)co_await db.execute(
            "UPDATE sys_operation SET created_at = NOW() - INTERVAL '8 days' WHERE id = $1::uuid", std::string_view(id));
        (void)co_await db.execute(
            "UPDATE sys_operation_subtask SET created_at = NOW() - INTERVAL '8 days', closed_at = CASE WHEN state = "
            "'open' THEN NULL ELSE NOW() - INTERVAL '8 days' END WHERE operation_id = $1::uuid",
            std::string_view(id));
    }
    test_support::recorded_notifications pruner{.handle_ = db};
    co_await service::sync_runtime::prune_result_events(pruner);
    const auto kept = [&](std::string_view id) -> ruvia::task<bool> {
        co_return integer(co_await db.query("SELECT count(*) FROM sys_operation WHERE id = $1::uuid", id)) == 1;
    };
    require(!(co_await kept(operation_of(settled))), "a settled task created 8 days ago is deleted");
    require(co_await kept(operation_of(unsettled)), "an unsettled task stays");
    require(co_await kept(statuses.attention_), "a task with an attempt within 7 days stays");
    require(pruner.sent_.size() == 1 && pruner.sent_[0].changes_.size() == 1 &&
                pruner.sent_[0].changes_[0].id_ == operation_of(settled),
        "deleted tasks are notified");

    // Once its attempts age out too, the history resets and the task goes.
    (void)co_await db.execute(
        "UPDATE sys_sync_event SET emitted_at = NOW() - INTERVAL '8 days' WHERE subtask_id IN (SELECT id FROM "
        "sys_operation_subtask WHERE operation_id = $1::uuid)",
        statuses.attention_);
    test_support::recorded_notifications second{.handle_ = db};
    co_await service::sync_runtime::prune_result_events(second);
    require(!(co_await kept(statuses.attention_)) && second.sent_.size() == 1 &&
                second.sent_[0].task_history_resets_.size() == 1 &&
                second.sent_[0].task_history_resets_[0] == statuses.attention_,
        "aged attempts reset the history, then the settled task is deleted");
    require(integer(co_await db.query(
                "SELECT count(*) FROM sys_sync_task WHERE dns_zone_id = $1 AND subtask_id IS NULL", zone(11))) == 1,
        "deleting a task releases its marker, which stays");
}

// A root belongs to the business transaction, independently of how many
// background operations it needs. Failed writes roll back the whole change.
ruvia::task<void> verify_user_operation_transactions(ruvia::db_handle& db, read_context& context) {
    const auto before = co_await db.query(
        "SELECT (SELECT count(*) FROM sys_operation), (SELECT count(*) FROM sys_operation_subtask), "
        "(SELECT count(*) FROM sys_sync_task), (SELECT name FROM sys_provider WHERE id = $1)",
        dns_provider);
    auto failed = user(action::provider_update, "provider", dns_provider);
    bool rejected = false;
    {
        auto transaction = co_await db.begin_transaction();
        try {
            (void)co_await transaction.execute("UPDATE sys_provider SET name = 'Rolled back' WHERE id = $1", dns_provider);
            (void)co_await failed.require_operation(transaction, tenant);
            (void)co_await service::sync_runtime::upsert_marker(transaction, tenant, marker_resource_type::provider,
                dns_provider, marker_operation::verify, 5000, failed);
            (void)co_await transaction.query("SELECT 1 / 0");
        } catch (const ruvia::db_error&) {
            rejected = true;
        }
        // SQL errors retire the transaction lease and roll back its writes.
        // Explicit rollback is only valid while the transaction is active.
        if (transaction.active()) {
            co_await transaction.rollback();
        }
    }
    const auto after = co_await db.query(
        "SELECT (SELECT count(*) FROM sys_operation), (SELECT count(*) FROM sys_operation_subtask), "
        "(SELECT count(*) FROM sys_sync_task), (SELECT name FROM sys_provider WHERE id = $1)",
        dns_provider);
    require(rejected && failed.operation_id().has_value() && integer(after, 0) == integer(before, 0) &&
                integer(after, 1) == integer(before, 1) && integer(after, 2) == integer(before, 2) &&
                text(after, 3) == text(before, 3) &&
                integer(co_await db.query("SELECT count(*) FROM sys_operation WHERE id = $1::uuid",
                    operation_of(failed))) == 0,
        "a database failure rolls back the business change, root, child and queued work together");

    auto first = user(action::cluster_update, "cluster", cluster);
    {
        auto write = co_await db.begin_transaction();
        const auto first_id = co_await first.require_operation(write, tenant);
        require(first_id == co_await first.require_operation(write, tenant),
            "requiring the operation twice keeps one root in the transaction");
        (void)co_await service::sync_runtime::upsert_marker(write, tenant, marker_resource_type::dns_zone,
            zone(15), marker_operation::sync, 5000, first);
        (void)co_await service::sync_runtime::upsert_marker(write, tenant, marker_resource_type::dns_zone,
            zone(16), marker_operation::sync, 5000, first);
        co_await write.commit();
    }
    const auto operation = co_await tasks().detail(context, std::string(tenant), operation_of(first));
    require(integer(co_await db.query("SELECT count(*) FROM sys_operation")) == integer(before, 0) + 1 &&
                operation.get<"action">().view() == "cluster.update" && operation.get<"subtasks">().size() == 2 &&
                operation.get<"subtask_count">().value_ == 2 &&
                subtask_in(operation, zone(15)).get<"kind">().view() == "dns_sync" &&
                subtask_in(operation, zone(16)).get<"kind">().view() == "dns_sync",
        "one user operation owns all required background work under exactly one root");

    auto second = user(action::cluster_update, "cluster", cluster);
    {
        auto write = co_await db.begin_transaction();
        (void)co_await second.require_operation(write, tenant);
        co_await write.commit();
    }
    const auto empty = co_await tasks().detail(context, std::string(tenant), operation_of(second));
    require(operation_of(first) != operation_of(second) &&
                integer(co_await db.query("SELECT count(*) FROM sys_operation")) == integer(before, 0) + 2 &&
                empty.get<"status">().view() == "completed" && empty.get<"subtasks">().empty() &&
                empty.get<"finished_at">().view() == empty.get<"created_at">().view(),
        "the next operation has its own completed root even when it needs no background work");
}

ruvia::task<void> verify_work(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    std::pmr::monotonic_buffer_resource pool;
    read_context context{.handle_ = client.handle(), .pool_ = &pool};
    co_await insert_fixture(db);
    const auto merges = co_await verify_merge_and_cancel(db, context);
    const auto statuses = co_await verify_statuses(db, context);
    co_await verify_list(context, statuses, merges);
    const auto owned = co_await verify_unowned(db, context);
    co_await verify_tenant_isolation(db, context, owned);
    co_await verify_certificate_children(db, context);
    co_await verify_retention(db, statuses);
    co_await verify_user_operation_transactions(db, context);
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
    service::utils::configure_secret_key(std::string(64, 'a'));
    const auto fresh = with_database(*base, fresh_database);
    const auto upgrade = with_database(*base, upgrade_database);
    const auto drift = with_database(*base, drift_database);
    const auto& all = service::config::schema_migrations;
    try {
        const auto through_traces = migrations_through("0024_task_attempt_traces");
        require(all.size() == through_traces.size() + 1 && all.back().id() == "0025_operation_tasks",
            "0025 is the newest migration");
        if (test_support::run_postgres_test(*base, recreate_databases) != 0 ||
            // New database: the full baseline, then a repeated run is a no-op.
            migrate(fresh, all, all.size(), "fresh install") != 0 || migrate(fresh, all, 0, "fresh repeat") != 0 ||
            test_support::run_postgres_test(fresh, verify_schema) != 0 ||
            // A 0024 database with scheduling state and retained history.
            migrate(upgrade, through_traces, through_traces.size(), "0024 install") != 0 ||
            test_support::run_postgres_test(upgrade, insert_upgrade_rows) != 0 ||
            test_support::run_postgres_test(upgrade, record_upgrade_fingerprint) != 0 ||
            migrate(upgrade, all, 1, "0025 upgrade") != 0 || migrate(upgrade, all, 0, "upgrade repeat") != 0 ||
            test_support::run_postgres_test(upgrade, verify_schema) != 0 ||
            test_support::run_postgres_test(upgrade, verify_upgraded_rows) != 0 ||
            test_support::run_postgres_test(upgrade, verify_old_server) != 0 ||
            // A drifted 0024 database rejects 0025 and keeps everything.
            migrate(drift, through_traces, through_traces.size(), "drift install") != 0 ||
            test_support::run_postgres_test(drift, break_owner_column) != 0) {
            return 1;
        }
        bool rejected = false;
        try {
            const ruvia::db_migrator migrator(drift, {.table_ = "schema_migrations"});
            (void)migrator.migrate(all);
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "a conflicting subtask_id column rejects 0025");
        if (test_support::run_postgres_test(drift, verify_drift_rolled_back) != 0 ||
            test_support::run_postgres_test(fresh, verify_work) != 0) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Operation task migration, ownership, statuses, retention and isolation passed\n";
    return 0;
}
