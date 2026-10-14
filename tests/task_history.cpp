#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/model_object.h>

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
#include "service/features/certificate/attempt_detail.h"
#include "service/features/dns_sync/attempt_detail.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/features/provider_verification/attempt_detail.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/website_dns/attempt_detail.h"
#include "support/postgres_test.h"

namespace {

using namespace std::string_view_literals;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("task history: " + std::string(message));
    }
}

// Typed views of the history response, written from the contract
// (docs/task-center.md) rather than from the producer's models.
RUVIA_MODEL(dns_record_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(line_code, ruvia::string), RUVIA_REQUIRED_FIELD(content, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64), RUVIA_REQUIRED_FIELD(priority, ruvia::int64, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(proxied, ruvia::bool_value));
RUVIA_MODEL(dns_update_input, RUVIA_REQUIRED_FIELD(before, dns_record_input),
    RUVIA_REQUIRED_FIELD(after, dns_record_input));
RUVIA_MODEL(dns_conflict_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(line_code, ruvia::string), RUVIA_REQUIRED_FIELD(local_content, ruvia::string),
    RUVIA_REQUIRED_FIELD(remote_content, ruvia::string));
RUVIA_MODEL(dns_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(created, ruvia::array<dns_record_input>),
    RUVIA_REQUIRED_FIELD(updated, ruvia::array<dns_update_input>),
    RUVIA_REQUIRED_FIELD(deleted, ruvia::array<dns_record_input>), RUVIA_REQUIRED_FIELD(created_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(updated_count, ruvia::int64), RUVIA_REQUIRED_FIELD(deleted_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(unchanged_count, ruvia::int64), RUVIA_REQUIRED_FIELD(conflicts, ruvia::array<dns_conflict_input>),
    RUVIA_REQUIRED_FIELD(conflict_count, ruvia::int64));
RUVIA_MODEL(certificate_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(operation, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider, ruvia::string), RUVIA_REQUIRED_FIELD(domains, ruvia::array<ruvia::string>),
    RUVIA_REQUIRED_FIELD(stage, ruvia::string), RUVIA_REQUIRED_FIELD(issuer, ruvia::string),
    RUVIA_REQUIRED_FIELD(not_before, ruvia::string), RUVIA_REQUIRED_FIELD(not_after, ruvia::string),
    RUVIA_REQUIRED_FIELD(serial_number, ruvia::string), RUVIA_REQUIRED_FIELD(fingerprint_sha256, ruvia::string));
RUVIA_MODEL(website_check_input, RUVIA_REQUIRED_FIELD(hostname, ruvia::string),
    RUVIA_REQUIRED_FIELD(query_name, ruvia::string), RUVIA_REQUIRED_FIELD(expected, ruvia::string),
    RUVIA_REQUIRED_FIELD(resolved, ruvia::array<ruvia::string>), RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string));
RUVIA_MODEL(website_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(expected_target, ruvia::string),
    RUVIA_REQUIRED_FIELD(domain_count, ruvia::int64), RUVIA_REQUIRED_FIELD(checked_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(domains, ruvia::array<website_check_input>));
RUVIA_MODEL(provider_zone_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(status, ruvia::string));
RUVIA_MODEL(provider_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(provider, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider_kind, ruvia::string), RUVIA_REQUIRED_FIELD(zone_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(zones, ruvia::array<provider_zone_input>), RUVIA_REQUIRED_FIELD(directory_url, ruvia::string),
    RUVIA_REQUIRED_FIELD(external_account, ruvia::string));
RUVIA_MODEL(node_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(node_id, ruvia::string), RUVIA_REQUIRED_FIELD(node_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(agent_version, ruvia::string), RUVIA_REQUIRED_FIELD(source, ruvia::string),
    RUVIA_REQUIRED_FIELD(result, ruvia::string), RUVIA_REQUIRED_FIELD(phase, ruvia::string),
    RUVIA_REQUIRED_FIELD(error_code, ruvia::string), RUVIA_REQUIRED_FIELD(retryable, ruvia::bool_value));
RUVIA_MODEL(none_detail_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value));

constexpr std::string_view fresh_database = "task_history_fresh_auth_qa";
constexpr std::string_view upgrade_database = "task_history_upgrade_auth_qa";
constexpr std::string_view worker = "task-history-qa";
constexpr std::string_view tenant = "00000000-0000-4000-8000-0000000000a1";
constexpr std::string_view other_tenant = "00000000-0000-4000-8000-0000000000a2";
constexpr std::string_view dns_provider = "00000000-0000-4000-8000-0000000000b1";
constexpr std::string_view certificate_provider = "00000000-0000-4000-8000-0000000000b2";
constexpr std::string_view zone = "00000000-0000-4000-8000-0000000000c1";
constexpr std::string_view summary_zone = "00000000-0000-4000-8000-0000000000c2";
constexpr std::string_view certificate = "00000000-0000-4000-8000-0000000000d1";
constexpr std::string_view cluster = "00000000-0000-4000-8000-0000000000e1";
constexpr std::string_view website = "00000000-0000-4000-8000-0000000000f1";
constexpr std::string_view release = "00000000-0000-4000-8000-000000000081";
constexpr std::string_view node_a = "00000000-0000-4000-8000-000000000091";
constexpr std::string_view node_b = "00000000-0000-4000-8000-000000000092";
constexpr std::string_view dns_marker = "00000000-0000-4000-8000-000000000071";
constexpr std::string_view certificate_marker = "00000000-0000-4000-8000-000000000072";
constexpr std::string_view website_marker = "00000000-0000-4000-8000-000000000073";
constexpr std::string_view provider_marker = "00000000-0000-4000-8000-000000000074";
constexpr std::string_view summary_marker = "00000000-0000-4000-8000-000000000075";
// Legacy rows recorded before migration 0023 (task id is not a live marker).
constexpr std::string_view legacy_task = "00000000-0000-4000-8000-000000000061";
constexpr std::string_view legacy_resource = "00000000-0000-4000-8000-000000000062";

// What task_service needs from a request context.
struct read_context final {
    ruvia::db_handle handle_;
    std::pmr::memory_resource* pool_;

    ruvia::db_handle& db() {
        return handle_;
    }
    [[nodiscard]] std::pmr::memory_resource* pool() const {
        return pool_;
    }
};

ruvia::db_config with_database(const ruvia::db_config& base, std::string_view database) {
    auto config = base;
    config.database_ = std::string(database);
    return config;
}

// Migrations up to, but excluding, 0023: the state an existing database had
// before attempt details. Later migrations stay part of the full list.
std::vector<ruvia::db_migration> migrations_before_attempt_details() {
    const auto& all = service::config::schema_migrations;
    const auto found = std::find_if(all.begin(), all.end(),
        [](const auto& migration) { return migration.id() == "0023_task_attempt_details"; });
    require(found != all.end(), "0023 is part of the migrations");
    return {all.begin(), found};
}

std::int64_t integer(const ruvia::db_rows& rows, std::size_t column = 0) {
    return rows.front()[column].as<std::int64_t>().value_or(-1);
}

std::string text(const ruvia::db_rows& rows, std::size_t column = 0) {
    return std::string(rows.front()[column].value().value_or(""));
}

template <typename model_type>
model_type parse_detail(const service::task::task_attempt_dto& item) {
    const auto view = item.get<"detail">().view();
    auto parsed = ruvia::from_json<model_type>(view);
    require(parsed.has_value(), "detail matches its contract: " + std::string(view));
    return std::move(*parsed);
}

// Words that must never appear in a stored detail: credentials, keys,
// certificate material and configuration version numbers.
void require_no_sensitive_terms(std::string_view detail) {
    for (const auto* term : {"secret", "private", "envelope", "PEM", "BEGIN", "hmac", "key_id", "email",
             "account_id", "revision", "generation", "digest", "password", "token"}) {
        require(detail.find(term) == std::string_view::npos,
            "detail leaks a sensitive term '" + std::string(term) + "': " + std::string(detail));
    }
}

ruvia::task<void> recreate_databases(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    for (const auto database : {fresh_database, upgrade_database}) {
        (void)co_await db.execute("DROP DATABASE IF EXISTS " + std::string(database) + " WITH (FORCE)");
        (void)co_await db.execute("CREATE DATABASE " + std::string(database));
    }
}

// Column, constraint and index shape of a migrated sys_sync_event.
ruvia::task<void> verify_schema(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto columns = co_await db.query(
        "SELECT string_agg(column_name || ':' || data_type || ':' || is_nullable, ',' ORDER BY column_name) "
        "FROM information_schema.columns WHERE table_name = 'sys_sync_event' AND column_name IN "
        "('attempt','started_at','duration_ms','next_attempt_at','detail')");
    require(text(columns) ==
                "attempt:integer:NO,detail:jsonb:NO,duration_ms:bigint:NO,next_attempt_at:timestamp with time "
                "zone:YES,started_at:timestamp with time zone:YES",
        "attempt columns: " + text(columns));
    const auto indexes = co_await db.query(
        "SELECT string_agg(indexname, ',' ORDER BY indexname) FROM pg_indexes WHERE tablename = 'sys_sync_event'");
    require(text(indexes).find("idx_sync_event_task_resource_version") != std::string::npos &&
                text(indexes).find("idx_sync_event_task_version,") == std::string::npos &&
                !text(indexes).ends_with("idx_sync_event_task_version"),
        "attempt lookup index replaced: " + text(indexes));
    const auto checks = co_await db.query(
        "SELECT pg_get_constraintdef(oid) FROM pg_constraint WHERE conname = 'ck_sync_event_resource_type'");
    require(text(checks).find("'node'") != std::string::npos, "node attempts allowed");
    const auto defaults = co_await db.query(
        "SELECT column_default FROM information_schema.columns WHERE table_name = 'sys_sync_event' "
        "AND column_name = 'detail'");
    require(text(defaults).find("none") != std::string::npos, "detail default is none: " + text(defaults));
}

ruvia::task<void> insert_legacy_rows(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    (void)co_await db.execute(
        "INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'History QA', 'history-qa'), "
        "($2, 'History other', 'history-other')",
        tenant, other_tenant);
    // Two versions of one task plus another tenant, recorded before 0023.
    (void)co_await db.execute(
        "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, outcome, "
        "error, emitted_at) VALUES "
        "($1, $3, 'dns_zone', $4, 'sync', 1, 'failed', 'legacy one', NOW() - INTERVAL '3 hours'), "
        "($1, $3, 'dns_zone', $4, 'sync', 1, 'failed', 'legacy two', NOW() - INTERVAL '2 hours'), "
        "($1, $3, 'dns_zone', $4, 'sync', 1, 'completed', '', NOW() - INTERVAL '1 hour'), "
        "($1, $3, 'dns_zone', $4, 'sync', 2, 'failed', 'legacy next', NOW() - INTERVAL '30 minutes'), "
        "($2, $3, 'dns_zone', $4, 'sync', 1, 'completed', '', NOW() - INTERVAL '10 minutes')",
        tenant, other_tenant, legacy_task, legacy_resource);
    const auto rejected = co_await db.query(
        "SELECT count(*) FROM pg_constraint WHERE conname = 'ck_sync_event_resource_type' AND "
        "pg_get_constraintdef(oid) LIKE '%node%'");
    require(integer(rejected) == 0, "node attempts are new in 0023");
}

ruvia::task<void> verify_legacy_rows(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    const auto rows = co_await db.query(
        "SELECT string_agg(version || '/' || attempt || '/' || outcome || '/' || (started_at IS NULL) || '/' || "
        "duration_ms || '/' || (next_attempt_at IS NULL) || '/' || (detail = '{\"kind\":\"none\",\"truncated\":false}'), "
        "',' ORDER BY id) FROM sys_sync_event WHERE task_id = $1 AND tenant_id = $2",
        legacy_task, tenant);
    require(text(rows) ==
                "1/1/failed/true/0/true/true,1/2/failed/true/0/true/true,1/3/completed/true/0/true/true,"
                "2/1/failed/true/0/true/true",
        "legacy rows numbered per version with unknown metadata: " + text(rows));
    const auto other = co_await db.query("SELECT attempt FROM sys_sync_event WHERE tenant_id = $1", other_tenant);
    require(integer(other) == 1, "legacy numbering is per tenant");
    bool rejected = false;
    try {
        (void)co_await db.execute(
            "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, "
            "outcome, attempt) VALUES ($1, $2, 'dns_zone', $3, 'sync', 1, 'failed', 0)",
            tenant, legacy_task, legacy_resource);
    } catch (const ruvia::db_error&) {
        rejected = true;
    }
    require(rejected, "attempt numbers start at 1");
    rejected = false;
    try {
        (void)co_await db.execute(
            "INSERT INTO sys_sync_event(tenant_id, task_id, resource_type, resource_id, operation, version, "
            "outcome, attempt, detail) VALUES ($1, $2, 'dns_zone', $3, 'sync', 1, 'failed', 1, '[]')",
            tenant, legacy_task, legacy_resource);
    } catch (const ruvia::db_error&) {
        rejected = true;
    }
    require(rejected, "detail must be an object");
}

ruvia::task<void> insert_fixture(ruvia::db_handle& db) {
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id) VALUES "
        "($1, $2, 'dns', 'cloudflare', 'History DNS', 'qa-account')",
        dns_provider, tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider) VALUES ($1, $2, 'certificate', 'letsencrypt')",
        certificate_provider, tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain) VALUES "
        "($1, $3, $4, 'history.example.com'), ($2, $3, $4, 'summary.example.com')",
        zone, summary_zone, tenant, dns_provider);
    (void)co_await db.execute(
        "INSERT INTO sys_certificate(id, tenant_id, provider_id, dns_zone_id, domain) "
        "VALUES ($1, $2, $3, $4, 'www.history.example.com')",
        certificate, tenant, certificate_provider, zone);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(id, tenant_id, dns_zone_id, name, hostname_prefix) VALUES "
        "($1, $2, $3, 'History cluster', 'edge')",
        cluster, tenant, zone);
    (void)co_await db.execute("INSERT INTO sys_website(id, tenant_id, cluster_id, config) VALUES ($1, $2, $3, '{}')", website,
        tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, runtime, agent_id, node_secret_hash, "
        "node_secret_envelope) VALUES ($1, $3, $4, 'History node A', '{\"agent_version\":\"2.4.0\"}', "
        "repeat('a', 32), repeat('c', 64), 'qa-only'), ($2, $3, $4, 'History node B', '{}', repeat('b', 32), "
        "repeat('d', 64), 'qa-only')",
        node_a, node_b, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_cluster_release(id, tenant_id, cluster_id, generation, status, manifest_digest, "
        "manifest_envelope, schema_version) VALUES ($1, $2, $3, 3, 'active', repeat('a', 64), 'qa-only', 6)",
        release, tenant, cluster);
    (void)co_await db.execute(
        "INSERT INTO sys_node_release_target(tenant_id, release_id, node_id, status) VALUES "
        "($1, $2, $3, 'pending'), ($1, $2, $4, 'pending')",
        tenant, release, node_a, node_b);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, dns_zone_id, operation, version) VALUES "
        "($1, $3, 'dns_zone', $4, 'sync', 4), ($2, $3, 'dns_zone', $5, 'sync', 1)",
        dns_marker, summary_marker, tenant, zone, summary_zone);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, certificate_id, operation, "
        "version) VALUES ($1, $2, 'certificate', $3, 'renew', 2)",
        certificate_marker, tenant, certificate);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, website_id, operation, "
        "version) VALUES ($1, $2, 'website', $3, 'apply', 5)",
        website_marker, tenant, website);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(id, tenant_id, resource_type, provider_id, operation, "
        "version) VALUES ($1, $2, 'provider', $3, 'verify', 1)",
        provider_marker, tenant, dns_provider);
}

ruvia::task<void> lease(ruvia::db_handle& db, std::string_view marker) {
    (void)co_await db.execute(
        "UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 "
        "seconds' WHERE id = $1",
        marker, worker);
}

// Records one attempt through the production marker transition and commits it.
ruvia::task<service::sync_runtime::running_result_transition> record_attempt(ruvia::db_handle& db,
    const service::sync_runtime::running_marker_lease& running, bool completed, std::string_view detail,
    std::string_view error = "qa failure") {
    co_await lease(db, running.marker_.marker_id_);
    auto transaction = co_await db.begin_transaction();
    const auto transition =
        completed ? co_await service::sync_runtime::complete_running_and_record_event(transaction, running, detail)
                  : co_await service::sync_runtime::fail_running_and_record_event(transaction, running, error, detail);
    co_await transaction.commit();
    co_return transition;
}

service::sync_runtime::running_marker_lease running(std::string_view marker, std::int64_t version,
    service::sync_runtime::marker_resource_type type, std::string_view resource,
    std::chrono::steady_clock::time_point started) {
    return service::sync_runtime::make_running_lease(tenant, marker, version, worker, started, type, resource);
}

ruvia::task<service::task::task_history_dto> history(read_context& context, std::string_view task,
    std::string_view resource, std::int64_t version) {
    co_return co_await service::task::get_task_service().history(context, std::string(tenant), std::string(task),
        std::string(resource), version);
}

const service::task::task_attempt_dto& newest(const service::task::task_history_dto& data) {
    require(!data.get<"list">().empty(), "history has attempts");
    return data.get<"list">()[0];
}

ruvia::task<void> verify_dns_sync(ruvia::db_handle& db, read_context& context, std::pmr::memory_resource* pool) {
    using service::dns_sync::detail::attempt_record;
    service::dns_sync::detail::zone_sync_attempt failed;
    failed.record_deleted({.name_ = "old", .type_ = "A", .line_code_ = "default", .content_ = "192.0.2.1", .ttl_ = 300, .priority_ = std::nullopt, .proxied_ = false});
    const auto started = std::chrono::steady_clock::now() - std::chrono::milliseconds(1500);
    const auto first = co_await record_attempt(db,
        running(dns_marker, 4, service::sync_runtime::marker_resource_type::dns_zone, zone, started), false,
        failed.to_json(pool), "Cloudflare 返回异常");
    require(first.marker_transitioned_ && first.event_recorded_, "dns failure recorded");

    service::dns_sync::detail::zone_sync_attempt completed;
    completed.record_deleted({.name_ = "old", .type_ = "A", .line_code_ = "default", .content_ = "192.0.2.1", .ttl_ = 300, .priority_ = std::nullopt, .proxied_ = false});
    completed.record_created({.name_ = "www", .type_ = "CNAME", .line_code_ = "default", .content_ = "edge.history.example.com", .ttl_ = 600, .priority_ = std::nullopt, .proxied_ = true});
    completed.record_updated({.name_ = "@", .type_ = "MX", .line_code_ = "default", .content_ = "mx1.example.net", .ttl_ = 600, .priority_ = 20, .proxied_ = false},
        {.name_ = "@", .type_ = "MX", .line_code_ = "default", .content_ = "mx1.example.net", .ttl_ = 300, .priority_ = 10, .proxied_ = false});
    completed.record_unchanged();
    completed.record_unchanged();
    const auto second = co_await record_attempt(db,
        running(dns_marker, 4, service::sync_runtime::marker_resource_type::dns_zone, zone,
            std::chrono::steady_clock::now()),
        true, completed.to_json(pool));
    require(second.event_recorded_, "dns completion recorded");

    const auto data = co_await history(context, dns_marker, zone, 4);
    const auto& list = data.get<"list">();
    require(list.size() == 2 && !data.get<"truncated">().value_, "two dns attempts");
    const auto& success = list[0];
    const auto& failure = list[1];
    require(success.get<"attempt">().value_ == 2 && failure.get<"attempt">().value_ == 1, "attempt order");
    require(success.get<"outcome">().view() == "completed" && success.get<"error">().view().empty() &&
                success.get<"next_attempt_at">().view().empty(),
        "completion fields");
    require(failure.get<"outcome">().view() == "failed" && failure.get<"error">().view() == "Cloudflare 返回异常" &&
                !failure.get<"next_attempt_at">().view().empty(),
        "failure keeps error and retry time");
    require(failure.get<"duration_ms">().value_ >= 1500 && failure.get<"duration_ms">().value_ < 60'000 &&
                !failure.get<"started_at">().view().empty(),
        "duration from the claim instant");
    const auto timing = co_await db.query(
        "SELECT count(*) FROM sys_sync_event WHERE task_id = $1 AND started_at + duration_ms * INTERVAL '1 "
        "millisecond' = emitted_at AND (next_attempt_at IS NULL) = (outcome = 'completed')",
        dns_marker);
    require(integer(timing) == 2, "started_at + duration_ms = emitted_at; retry only on failure");
    const auto retry = co_await db.query(
        "SELECT count(*) FROM sys_sync_event event JOIN sys_sync_task marker ON marker.id = event.task_id "
        "WHERE event.task_id = $1 AND event.outcome = 'failed' AND event.next_attempt_at > event.emitted_at",
        dns_marker);
    require(integer(retry) == 1, "failure records the scheduled retry");

    const auto partial = parse_detail<dns_detail_input>(failure);
    require(partial.get<"kind">().view() == "dns_sync" && partial.get<"deleted_count">().value_ == 1 &&
                partial.get<"created">().empty(),
        "failed attempt keeps completed provider work");
    const auto detail = parse_detail<dns_detail_input>(success);
    require(detail.get<"created_count">().value_ == 1 && detail.get<"updated_count">().value_ == 1 &&
                detail.get<"deleted_count">().value_ == 1 && detail.get<"unchanged_count">().value_ == 2 &&
                detail.get<"conflict_count">().value_ == 0 && !detail.get<"truncated">().value_,
        "dns counts");
    require(detail.get<"created">()[0].get<"name">().view() == "www" &&
                detail.get<"created">()[0].get<"proxied">().value_ &&
                !detail.get<"created">()[0].get<"priority">().has_value(),
        "created record");
    require(detail.get<"updated">()[0].get<"before">().get<"ttl">().value_ == 600 &&
                detail.get<"updated">()[0].get<"after">().get<"ttl">().value_ == 300 &&
                *detail.get<"updated">()[0].get<"after">().get<"priority">() == 10,
        "updated before and after");
    require_no_sensitive_terms(success.get<"detail">().view());

    // A conflicting sync completes without provider changes and lists the conflicts.
    service::dns_sync::detail::zone_sync_attempt conflicting;
    conflicting.record_conflicts({{.id_ = "local-id", .type_ = "A", .name_ = "api", .line_code_ = "default", .local_content_ = "192.0.2.10", .remote_content_ = "（远端已删除）"}});
    (void)co_await record_attempt(db,
        running(dns_marker, 4, service::sync_runtime::marker_resource_type::dns_zone, zone,
            std::chrono::steady_clock::now()),
        true, conflicting.to_json(pool));
    const auto conflict_data = co_await history(context, dns_marker, zone, 4);
    const auto conflicts = parse_detail<dns_detail_input>(newest(conflict_data));
    require(conflicts.get<"conflict_count">().value_ == 1 &&
                conflicts.get<"conflicts">()[0].get<"remote_content">().view() == "（远端已删除）" &&
                conflicts.get<"created_count">().value_ == 0,
        "conflict detail");
}

ruvia::task<void> verify_certificate(ruvia::db_handle& db, read_context& context, std::pmr::memory_resource* pool) {
    using service::certificate_issuance::certificate_attempt;
    using service::certificate_issuance::certificate_stage;
    certificate_attempt failed{.operation_ = "renew",
        .provider_ = "letsencrypt",
        .domains_ = {"www.history.example.com", "*.history.example.com"},
        .stage_ = certificate_stage::dns_challenge,
        .issued_ = std::nullopt};
    (void)co_await record_attempt(db,
        running(certificate_marker, 2, service::sync_runtime::marker_resource_type::certificate, certificate,
            std::chrono::steady_clock::now()),
        false, failed.to_json(pool), "DNS 同步等待超时");
    auto completed = failed;
    completed.stage_ = certificate_stage::completed;
    completed.issued_ = service::certificate_issuance::issued_certificate_summary{.issuer_ = "CN=R11,O=Let's Encrypt,C=US",
        .not_before_ = "2026-10-14T00:00:00Z",
        .not_after_ = "2027-01-12T00:00:00Z",
        .serial_number_ = "0A1B2C",
        .fingerprint_sha256_ = "AA:BB:CC"};
    (void)co_await record_attempt(db,
        running(certificate_marker, 2, service::sync_runtime::marker_resource_type::certificate, certificate,
            std::chrono::steady_clock::now()),
        true, completed.to_json(pool));
    const auto data = co_await history(context, certificate_marker, certificate, 2);
    const auto success = parse_detail<certificate_detail_input>(data.get<"list">()[0]);
    const auto failure = parse_detail<certificate_detail_input>(data.get<"list">()[1]);
    require(failure.get<"stage">().view() == "dns_challenge" && failure.get<"issuer">().view().empty() &&
                failure.get<"domains">().size() == 2 && failure.get<"operation">().view() == "renew",
        "failed issuance reports its stage");
    require(success.get<"stage">().view() == "completed" &&
                success.get<"issuer">().view() == "CN=R11,O=Let's Encrypt,C=US" &&
                success.get<"not_after">().view() == "2027-01-12T00:00:00Z" &&
                success.get<"provider">().view() == "letsencrypt",
        "issued certificate summary");
    require_no_sensitive_terms(data.get<"list">()[0].get<"detail">().view());
    // A stage other than completed never carries issuance results.
    auto stale = completed;
    stale.stage_ = certificate_stage::persist;
    require(stale.to_json(pool).find("Let's Encrypt") == std::string::npos, "issuer only when completed");
}

ruvia::task<void> verify_website(ruvia::db_handle& db, read_context& context, std::pmr::memory_resource* pool) {
    service::website_dns::website_domain_attempt attempt;
    attempt.start("edge.history.example.com", 3);
    attempt.record_check({.hostname_ = "www.history.example.com",
        .query_name_ = "www.history.example.com",
        .resolved_ = {"edge.history.example.com"},
        .status_ = "verified",
        .error_ = {}});
    attempt.record_check({.hostname_ = "*.img.history.example.com",
        .query_name_ = "flexedge-00000000.img.history.example.com",
        .resolved_ = {"other.example.net"},
        .status_ = "invalid",
        .error_ = "期望 edge.history.example.com，实际 other.example.net"});
    (void)co_await record_attempt(db,
        running(website_marker, 5, service::sync_runtime::marker_resource_type::website, website,
            std::chrono::steady_clock::now()),
        false, attempt.to_json(pool), "网站同步标记 lease 已失效");
    const auto data = co_await history(context, website_marker, website, 5);
    const auto detail = parse_detail<website_detail_input>(newest(data));
    require(detail.get<"domain_count">().value_ == 3 && detail.get<"checked_count">().value_ == 2 &&
                detail.get<"domains">().size() == 2,
        "partial website check is kept");
    require(detail.get<"domains">()[1].get<"status">().view() == "invalid" &&
                detail.get<"domains">()[1].get<"resolved">()[0].view() == "other.example.net" &&
                detail.get<"domains">()[0].get<"expected">().view() == "edge.history.example.com",
        "per-domain result");
}

ruvia::task<void> verify_provider(ruvia::db_handle& db, read_context& context, std::pmr::memory_resource* pool) {
    service::provider_verification::detail::provider_attempt attempt("cloudflare", "dns");
    attempt.record_zone("history.example.com", "active");
    attempt.record_zone("summary.example.com", "pending");
    (void)co_await record_attempt(db,
        running(provider_marker, 1, service::sync_runtime::marker_resource_type::provider, dns_provider,
            std::chrono::steady_clock::now()),
        true, attempt.to_json(pool));
    const auto data = co_await history(context, provider_marker, dns_provider, 1);
    const auto detail = parse_detail<provider_detail_input>(newest(data));
    require(detail.get<"provider">().view() == "cloudflare" && detail.get<"provider_kind">().view() == "dns" &&
                detail.get<"zone_count">().value_ == 2 && detail.get<"zones">()[1].get<"status">().view() == "pending" &&
                detail.get<"external_account">().view().empty(),
        "provider summary");
    require_no_sensitive_terms(newest(data).get<"detail">().view());
    service::provider_verification::detail::provider_attempt certificate_attempt("zerossl", "certificate");
    certificate_attempt.record_directory("https://acme.zerossl.com/v2/DV90", "fetched");
    const auto certificate_json = certificate_attempt.to_json(pool);
    require(certificate_json.find(R"("external_account":"fetched")") != std::string::npos &&
                certificate_json.find(R"("zones":[])") != std::string::npos,
        "certificate provider summary");
}

ruvia::task<void> verify_node_apply(ruvia::db_handle& db, read_context& context, std::pmr::memory_resource* pool) {
    // Mirrors agent_command_service: the target transition, then its attempt.
    const auto record = [&](service::node_dispatch::node_apply_attempt attempt) -> ruvia::task<bool> {
        auto transaction = co_await db.begin_transaction();
        const auto changed =
            attempt.applied_
                ? co_await service::node_dispatch::mark_node_target_applied(transaction, attempt.tenant_id_,
                      attempt.release_id_, attempt.node_id_)
                : co_await service::node_dispatch::mark_node_target_failed(transaction, attempt.tenant_id_,
                      attempt.release_id_, attempt.node_id_, cluster,
                      {.phase_ = std::string(attempt.phase_), .error_code_ = std::string(attempt.error_code_), .error_ = std::string(attempt.error_), .retryable_ = attempt.retryable_});
        const auto recorded =
            changed && co_await service::node_dispatch::record_node_apply_attempt(transaction, attempt, pool);
        co_await transaction.commit();
        co_return recorded;
    };
    require(co_await record({.tenant_id_ = tenant,
                .release_id_ = release,
                .node_id_ = node_a,
                .source_ = "apply_result",
                .applied_ = false,
                .phase_ = "validate",
                .error_code_ = "artifact_invalid",
                .error_ = "配置校验失败",
                .retryable_ = false,
                .elapsed_milliseconds_ = 2500}),
        "node failure recorded");
    require(co_await record({.tenant_id_ = tenant,
                .release_id_ = release,
                .node_id_ = node_a,
                .source_ = "apply_result",
                .applied_ = true,
                .phase_ = "validate",
                .error_code_ = "ignored",
                .error_ = "ignored",
                .retryable_ = true,
                .elapsed_milliseconds_ = 900}),
        "node success recorded");
    require(co_await record({.tenant_id_ = tenant,
                .release_id_ = release,
                .node_id_ = node_b,
                .source_ = "heartbeat",
                .applied_ = true,
                .phase_ = {},
                .error_code_ = {},
                .error_ = {},
                .retryable_ = false,
                .elapsed_milliseconds_ = std::nullopt}),
        "heartbeat confirmation recorded");
    require(!(co_await record({.tenant_id_ = other_tenant,
                .release_id_ = release,
                .node_id_ = node_a,
                .source_ = "apply_result",
                .applied_ = true,
                .phase_ = {},
                .error_code_ = {},
                .error_ = {},
                .retryable_ = false,
                .elapsed_milliseconds_ = std::nullopt})),
        "no attempt without a release target of the tenant");

    const auto data = co_await history(context, release, node_a, 3);
    const auto& list = data.get<"list">();
    require(list.size() == 2, "node history narrowed to its node");
    const auto applied = parse_detail<node_detail_input>(list[0]);
    const auto failed = parse_detail<node_detail_input>(list[1]);
    require(list[0].get<"attempt">().value_ == 2 && list[0].get<"duration_ms">().value_ == 900 &&
                list[1].get<"error">().view() == "配置校验失败" && list[1].get<"next_attempt_at">().view().empty(),
        "node attempt metadata");
    require(applied.get<"result">().view() == "applied" && applied.get<"phase">().view().empty() &&
                applied.get<"error_code">().view().empty() && !applied.get<"retryable">().value_ &&
                applied.get<"node_name">().view() == "History node A" &&
                applied.get<"agent_version">().view() == "2.4.0",
        "applied node detail");
    require(failed.get<"result">().view() == "failed" && failed.get<"phase">().view() == "validate" &&
                failed.get<"error_code">().view() == "artifact_invalid" && failed.get<"source">().view() == "apply_result",
        "failed node detail");
    require_no_sensitive_terms(list[1].get<"detail">().view());
    const auto heartbeat = co_await history(context, release, node_b, 3);
    const auto confirmed = parse_detail<node_detail_input>(newest(heartbeat));
    require(confirmed.get<"source">().view() == "heartbeat" && newest(heartbeat).get<"started_at">().view().empty() &&
                newest(heartbeat).get<"duration_ms">().value_ == 0 && confirmed.get<"agent_version">().view().empty(),
        "heartbeat confirmation has no timing");
    const auto release_wide = co_await history(context, release, "", 3);
    require(release_wide.get<"list">().size() == 3 && release_wide.get<"summary">().get<"attempt_count">().value_ == 3,
        "without resource_id the release's attempts are combined");

    // The read model keeps one record per target and counts node failures.
    const auto records = co_await db.query(
        service::task::task_records_sql +
            "SELECT string_agg(resource_id::text || ':' || status || ':' || failures, ',' ORDER BY resource_id) "
            "FROM records WHERE resource_type = 'node'",
        tenant, std::string_view{}, std::string_view{}, std::string_view{}, 0);
    require(text(records) == std::string(node_a) + ":recovered:1," + std::string(node_b) + ":completed:0",
        "node attempts never become orphan task records: " + text(records));
    const auto orphans = co_await db.query(
        service::task::task_records_sql + "SELECT count(*) FROM records WHERE id = $6",
        tenant, std::string_view{}, std::string_view{}, std::string_view{}, 0, release);
    require(integer(orphans) == 2, "release appears once per target");
}

ruvia::task<void> verify_summary_beyond_limit(ruvia::db_handle& db, read_context& context,
    std::pmr::memory_resource* pool) {
    service::dns_sync::detail::zone_sync_attempt empty;
    const auto detail = empty.to_json(pool);
    for (int index = 0; index < 120; ++index) {
        (void)co_await record_attempt(db,
            running(summary_marker, 1, service::sync_runtime::marker_resource_type::dns_zone, summary_zone,
                std::chrono::steady_clock::now()),
            false, detail);
    }
    (void)co_await record_attempt(db,
        running(summary_marker, 1, service::sync_runtime::marker_resource_type::dns_zone, summary_zone,
            std::chrono::steady_clock::now()),
        true, detail);
    (void)co_await record_attempt(db,
        running(summary_marker, 1, service::sync_runtime::marker_resource_type::dns_zone, summary_zone,
            std::chrono::steady_clock::now()),
        false, detail);
    const auto data = co_await history(context, summary_marker, summary_zone, 1);
    const auto& summary = data.get<"summary">();
    const auto& list = data.get<"list">();
    require(list.size() == 100 && data.get<"truncated">().value_, "list stays at 100");
    require(summary.get<"attempt_count">().value_ == 122 && summary.get<"failure_count">().value_ == 121,
        "summary counts every retained attempt");
    require(list[0].get<"attempt">().value_ == 122 && list[99].get<"attempt">().value_ == 23, "contiguous numbering");
    const auto first = co_await db.query(
        "SELECT TO_CHAR(min(emitted_at), 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), TO_CHAR(max(emitted_at) FILTER "
        "(WHERE outcome = 'completed'), 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), (SELECT TO_CHAR(next_attempt_at, "
        "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF') FROM sys_sync_task WHERE id = $1) FROM sys_sync_event WHERE task_id = $1",
        summary_marker);
    require(summary.get<"first_failure_at">().view() == text(first, 0) &&
                summary.get<"last_success_at">().view() == text(first, 1),
        "first failure is older than the listed window, last success is retained");
    require(summary.get<"next_attempt_at">().view() == text(first, 2), "summary shows the scheduled retry");
    co_await lease(db, summary_marker);
    const auto running_data = co_await history(context, summary_marker, summary_zone, 1);
    require(running_data.get<"summary">().get<"next_attempt_at">().view().empty(),
        "no scheduled retry while an attempt runs");
    const auto other = co_await service::task::get_task_service().history(context, std::string(other_tenant),
        std::string(summary_marker), std::string(summary_zone), 1);
    require(other.get<"list">().empty() && other.get<"summary">().get<"attempt_count">().value_ == 0,
        "history is tenant scoped");
}

ruvia::task<void> verify_bounds_and_failures(ruvia::db_handle& db, read_context& context,
    std::pmr::memory_resource* pool) {
    const std::string long_content(400, 'x');
    service::dns_sync::detail::zone_sync_attempt large;
    for (int index = 0; index < 200; ++index) {
        large.record_created({.name_ = "host-" + std::to_string(index), .type_ = "TXT", .line_code_ = "default", .content_ = long_content, .ttl_ = 600, .priority_ = std::nullopt, .proxied_ = false});
        large.record_deleted({.name_ = "gone-" + std::to_string(index), .type_ = "TXT", .line_code_ = "default", .content_ = long_content, .ttl_ = 600, .priority_ = std::nullopt, .proxied_ = false});
    }
    const auto large_json = large.to_json(pool);
    require(large_json.size() <= service::sync_runtime::detail_max_bytes, "detail byte cap");
    const auto parsed = ruvia::from_json<dns_detail_input>(large_json);
    require(parsed && parsed->get<"truncated">().value_ && parsed->get<"created_count">().value_ == 200 &&
                parsed->get<"deleted_count">().value_ == 200 && parsed->get<"created">().size() <= 50 &&
                parsed->get<"created">()[0].get<"content">().view().size() == service::sync_runtime::detail_text_bytes,
        "lists and text are truncated with exact counts");

    bool cut = false;
    const auto bounded = service::sync_runtime::bounded_detail_text(
        "a\0b\xff"
        "c错误"sv,
        cut, 6);
    require(bounded == "a bc" && cut, "control characters, invalid UTF-8 and partial characters are removed");

    // Unusable details are replaced; the marker transition still commits.
    std::string oversized = R"({"kind":"dns_sync","truncated":false,"pad":")";
    oversized.append(service::sync_runtime::detail_max_bytes, 'p');
    oversized += "\"}";
    for (const auto& unusable : {std::string("not json"), std::string(R"({"kind":"x","truncated":false,"v":"\u0000"})"),
             std::string(R"({"truncated":false})"), oversized}) {
        const auto transition = co_await record_attempt(db,
            running(dns_marker, 4, service::sync_runtime::marker_resource_type::dns_zone, zone,
                std::chrono::steady_clock::now()),
            false, unusable, "detail rejected");
        require(transition.marker_transitioned_ && transition.event_recorded_, "result survives an unusable detail");
        const auto data = co_await history(context, dns_marker, zone, 4);
        const auto none = parse_detail<none_detail_input>(newest(data));
        require(none.get<"kind">().view() == "none" && none.get<"truncated">().value_ &&
                    newest(data).get<"error">().view() == "detail rejected",
            "dropped detail marker");
    }
    const auto marker = co_await db.query("SELECT is_done, count_fails, error FROM sys_sync_task WHERE id = $1",
        dns_marker);
    require(!marker.front()[0].as<bool>().value_or(true) && text(marker, 2) == "detail rejected",
        "marker failure applied with every dropped detail");
    const auto thrown = service::sync_runtime::serialize_detail(pool,
        [](std::pmr::memory_resource*) -> service::node_dispatch::node_apply_detail_dto {
            throw std::runtime_error("mapping failed");
        });
    require(thrown == service::sync_runtime::detail_dropped_json, "a failing detail builder is dropped");

    // An attempt with an unknown start keeps started_at empty and duration 0.
    (void)co_await record_attempt(db,
        running(dns_marker, 4, service::sync_runtime::marker_resource_type::dns_zone, zone,
            std::chrono::steady_clock::time_point{}),
        false, service::sync_runtime::detail_none_json);
    const auto unknown = co_await history(context, dns_marker, zone, 4);
    require(newest(unknown).get<"started_at">().view().empty() && newest(unknown).get<"duration_ms">().value_ == 0,
        "unknown start");
}

ruvia::task<void> verify_records(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    std::pmr::monotonic_buffer_resource pool;
    read_context context{.handle_ = client.handle(), .pool_ = &pool};
    co_await insert_fixture(db);
    co_await verify_dns_sync(db, context, &pool);
    co_await verify_certificate(db, context, &pool);
    co_await verify_website(db, context, &pool);
    co_await verify_provider(db, context, &pool);
    co_await verify_node_apply(db, context, &pool);
    co_await verify_summary_beyond_limit(db, context, &pool);
    co_await verify_bounds_and_failures(db, context, &pool);

    // Pre-0023 rows read back with the legacy defaults.
    const auto legacy = co_await history(context, legacy_task, legacy_resource, 1);
    require(legacy.get<"list">().size() == 3, "legacy history size");
    const auto legacy_detail = parse_detail<none_detail_input>(legacy.get<"list">()[0]);
    require(legacy.get<"summary">().get<"failure_count">().value_ == 2 &&
                legacy.get<"list">()[0].get<"attempt">().value_ == 3 &&
                legacy.get<"list">()[0].get<"started_at">().view().empty() &&
                legacy_detail.get<"kind">().view() == "none" && !legacy_detail.get<"truncated">().value_,
        "legacy history");

    // Retention still removes attempts older than 7 days, node attempts included.
    (void)co_await db.execute("UPDATE sys_sync_event SET emitted_at = NOW() - INTERVAL '8 days' WHERE task_id = $1",
        release);
    co_await service::sync_runtime::prune_result_events(db);
    const auto pruned = co_await db.query("SELECT count(*) FROM sys_sync_event WHERE task_id = $1", release);
    require(integer(pruned) == 0, "7 day retention");
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
    const auto& all = service::config::schema_migrations;
    const auto before = migrations_before_attempt_details();
    try {
        if (test_support::run_postgres_test(*base, recreate_databases) != 0 ||
            // New database: every migration, then a repeated run changes nothing.
            migrate(fresh, all, all.size(), "fresh install") != 0 || migrate(fresh, all, 0, "fresh repeat") != 0 ||
            test_support::run_postgres_test(fresh, verify_schema) != 0 ||
            // Existing database with retained attempts: 0023 and later, then a repeated run.
            migrate(upgrade, before, before.size(), "pre-0023 install") != 0 ||
            test_support::run_postgres_test(upgrade, insert_legacy_rows) != 0 ||
            migrate(upgrade, all, all.size() - before.size(), "0023 upgrade") != 0 ||
            migrate(upgrade, all, 0, "upgrade repeat") != 0 ||
            test_support::run_postgres_test(upgrade, verify_schema) != 0 ||
            test_support::run_postgres_test(upgrade, verify_legacy_rows) != 0 ||
            test_support::run_postgres_test(upgrade, verify_records) != 0) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Task history migration, recording and reads passed\n";
    return 0;
}
