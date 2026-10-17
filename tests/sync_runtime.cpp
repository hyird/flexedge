#include <ruvia/core/task_scope.h>
#include <ruvia/web/app.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "service/features/certificate/maintenance.h"
#include "service/features/dns_sync/maintenance.h"
#include "service/features/provider_verification/persistence.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/worker.h"
#include "service/features/website_dns/worker.h"
#include "support/postgres_test.h"
#include "support/recorded_notifications.h"


#ifdef _MSC_VER
#pragma optimize("", off)
#endif
namespace {

namespace sync = service::sync_runtime;
using service::background::task_context;

constexpr std::string_view tenant{"00000000-0000-4000-8000-000000000a01"};
constexpr std::string_view zone{"00000000-0000-4000-8000-000000000a02"};
constexpr std::string_view provider{"00000000-0000-4000-8000-000000000a03"};
constexpr std::string_view certificate{"00000000-0000-4000-8000-000000000a04"};
constexpr std::string_view website{"00000000-0000-4000-8000-000000000a05"};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("sync runtime: " + std::string(message));
    }
}

// Only the tables and columns these operations own or read. This ordinary
// behavior test deliberately does not install the application's full baseline.
ruvia::task<void> prepare(ruvia::db_handle db) {
    std::string_view schema{R"sql(
CREATE TABLE sys_provider (
    id uuid PRIMARY KEY, tenant_id uuid NOT NULL, verification_generation bigint DEFAULT 1,
    revision bigint DEFAULT 1, deleted_at timestamptz, updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_dns_zone (
    id uuid PRIMARY KEY, tenant_id uuid NOT NULL, desired_revision bigint DEFAULT 1,
    synced_revision bigint DEFAULT 0, revision bigint DEFAULT 1, deleted_at timestamptz,
    sort bigint DEFAULT 1, updated_at timestamptz DEFAULT NOW(),
    config jsonb DEFAULT '{"records":[]}', sync_status text DEFAULT 'pending');
CREATE TABLE sys_cluster (
    id uuid, tenant_id uuid, dns_zone_id uuid, hostname_prefix text,
    status text, deleted_at timestamptz);
CREATE TABLE sys_certificate (
    id uuid PRIMARY KEY, tenant_id uuid NOT NULL, issuance_revision bigint DEFAULT 2,
    issued_revision bigint DEFAULT 1, status text DEFAULT 'valid', expires_at timestamptz DEFAULT NOW() + INTERVAL '1 day',
    config jsonb DEFAULT '{"auto_renew":true}', deleted_at timestamptz, sort bigint DEFAULT 1,
    last_error text, updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_website (id uuid PRIMARY KEY, tenant_id uuid NOT NULL, revision bigint DEFAULT 1,
    deleted_at timestamptz, updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_sync_task (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id uuid NOT NULL, resource_type text NOT NULL,
    provider_id uuid, dns_zone_id uuid, certificate_id uuid, website_id uuid,
    resource_id uuid GENERATED ALWAYS AS (COALESCE(provider_id, dns_zone_id, certificate_id, website_id)) STORED,
    operation text NOT NULL, version bigint NOT NULL, processed_version bigint NOT NULL DEFAULT 0,
    is_done boolean NOT NULL DEFAULT FALSE, is_ok boolean NOT NULL DEFAULT FALSE,
    error text NOT NULL DEFAULT '', count_fails integer NOT NULL DEFAULT 0,
    next_attempt_at timestamptz NOT NULL DEFAULT NOW(), lease_owner text, lease_until timestamptz,
    wait_task_id uuid, wait_version bigint, created_at timestamptz NOT NULL DEFAULT NOW(),
    updated_at timestamptz NOT NULL DEFAULT NOW(),
    UNIQUE (tenant_id, resource_type, resource_id),
    CHECK (NOT is_ok OR is_done), CHECK ((lease_owner IS NULL) = (lease_until IS NULL)),
    CHECK ((wait_task_id IS NULL) = (wait_version IS NULL)),
    CHECK (version > 0 AND processed_version >= 0 AND processed_version <= version));
CREATE TABLE sys_sync_event (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, tenant_id uuid NOT NULL, task_id uuid NOT NULL,
    resource_type text NOT NULL, resource_id uuid NOT NULL, operation text NOT NULL, version bigint NOT NULL,
    outcome text NOT NULL CHECK (outcome IN ('completed', 'failed', 'progressed')),
    error text NOT NULL, attempt integer NOT NULL CHECK (attempt > 0),
    started_at timestamptz, duration_ms bigint NOT NULL,
    detail jsonb NOT NULL CHECK (jsonb_typeof(detail) = 'object'), emitted_at timestamptz NOT NULL);
CREATE TABLE sys_sync_event_trace (
    event_id bigint PRIMARY KEY REFERENCES sys_sync_event(id), tenant_id uuid NOT NULL,
    summary jsonb NOT NULL, trace jsonb NOT NULL, trace_bytes integer NOT NULL);
CREATE TABLE sys_notification_outbox (
    id varchar(64) PRIMARY KEY, kind varchar(24) NOT NULL, payload text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT NOW(), available_at timestamptz NOT NULL DEFAULT NOW(),
    attempts integer NOT NULL DEFAULT 0);
)sql"};
    while (!schema.empty()) {
        const auto end = schema.find(';');
        if (end == std::string_view::npos) {
            break;
        }
        (void)co_await db.execute(schema.substr(0, end));
        schema.remove_prefix(end + 1);
    }
    (void)co_await db.execute("INSERT INTO sys_provider(id, tenant_id) VALUES ($1, $2)", provider, tenant);
    (void)co_await db.execute("INSERT INTO sys_dns_zone(id, tenant_id) VALUES ($1, $2)", zone, tenant);
    (void)co_await db.execute("INSERT INTO sys_certificate(id, tenant_id) VALUES ($1, $2)", certificate, tenant);
    (void)co_await db.execute("INSERT INTO sys_website(id, tenant_id) VALUES ($1, $2)", website, tenant);
}

ruvia::task<std::string> enqueue(task_context& context, sync::marker_resource_type type,
    std::string_view resource, sync::marker_operation operation, std::int64_t version) {
    service::background::queued_work work;
    auto transaction = co_await context.db().begin_transaction();
    auto id = co_await sync::upsert_marker(transaction, tenant, type, resource, operation, version, work);
    co_await sync::commit_work(transaction, context, tenant, work);
    co_return id;
}

sync::running_marker_lease lease_of(task_context& context, const sync::claimed_marker& marker) {
    return sync::make_running_lease(marker.tenant_id_, marker.id_, marker.version_, context.lease_owner(),
        marker.claimed_at_, sync::marker_resource_type::dns_zone, marker.resource_id_);
}

ruvia::task<sync::task_result> finish(task_context& context, const sync::running_marker_lease& lease,
    const sync::attempt_result& result) {
    auto transaction = co_await context.db().begin_transaction();
    sync::running_result_writer writer(transaction, lease);
    auto transition = co_await writer.record(result, sync::detail_none_json);
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web", lease.started_);
    co_await writer.commit(context, trace);
    co_return transition;
}

ruvia::task<std::int64_t> event_count(ruvia::db_handle db) {
    const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_sync_event");
    co_return rows.front()[0].as<std::int64_t>().value();
}

ruvia::task<void> verify_terminal_failures(task_context& context) {
    struct subject final {
        sync::marker_resource_type type_{};
        std::string_view id_{};
        sync::marker_operation operation_{};
        std::int64_t version_{};
    };
    const subject subjects[]{
        {.type_ = sync::marker_resource_type::dns_zone, .id_ = zone, .operation_ = sync::marker_operation::sync, .version_ = 1},
        {.type_ = sync::marker_resource_type::provider, .id_ = provider, .operation_ = sync::marker_operation::verify, .version_ = 1},
        {.type_ = sync::marker_resource_type::certificate, .id_ = certificate, .operation_ = sync::marker_operation::renew, .version_ = 2}};
    auto db = context.db();
    for (const auto& subject_value : subjects) {
        const auto id = co_await enqueue(context, subject_value.type_, subject_value.id_, subject_value.operation_, subject_value.version_);
        const auto marker = co_await sync::claim_marker(context, subject_value.type_);
        require(marker.has_value() && marker->id_ == id, "the production claim acquires the due task");
        const auto lease = sync::make_running_lease(marker->tenant_id_, marker->id_, marker->version_, context.lease_owner(),
            marker->claimed_at_, subject_value.type_, subject_value.id_);
        const auto transition = co_await finish(context, lease, sync::attempt_result::failed("invalid credentials"));
        require(transition.kind() == sync::result_kind::recorded && transition.event_id().has_value(), "terminal result and event commit together");
        const auto rows = co_await db.query(
            "SELECT task.is_done AND NOT task.is_ok, task.processed_version = task.version, task.count_fails, "
            "event.outcome = 'failed', task.lease_owner IS NULL AND task.wait_task_id IS NULL "
            "FROM sys_sync_task task JOIN sys_sync_event event ON event.task_id = task.id WHERE event.id = $1",
            *transition.event_id());
        require(rows.front()[0].as<bool>().value() && rows.front()[1].as<bool>().value() &&
                    rows.front()[2].as<int>().value() == 1 && rows.front()[3].as<bool>().value() && rows.front()[4].as<bool>().value(),
            "terminal failure records its outcome and releases its lease and dependency");
        (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 minute' WHERE id = $1", id);
        require(!(co_await sync::claim_marker(context, subject_value.type_)).has_value(), "an overdue terminal failure is never claimed");
    }
    co_await service::dns_sync::detail::reconcile_tasks(context);
    co_await service::certificate_issuance::worker_detail::reconcile_certificate_maintenance(context);
    co_await service::provider_verification::detail::reconcile(context);
    const auto rows = co_await db.query("SELECT COUNT(*) FROM sys_sync_task WHERE is_done AND NOT is_ok");
    require(rows.front()[0].as<int>().value() == 3, "maintenance preserves terminal failures, including an auto-renewable valid certificate");
    const auto issued = co_await db.query("SELECT issuance_revision FROM sys_certificate WHERE id = $1", certificate);
    require(issued.front()[0].as<int>().value() == 2, "automatic renewal cannot increment the failed issuance again");
}

ruvia::task<void> verify_retry_and_fences(task_context& first, task_context& second) {
    auto db = first.db();
    const auto previous = co_await db.query("SELECT id::text FROM sys_sync_task WHERE dns_zone_id = $1", zone);
    const auto id = co_await enqueue(first, sync::marker_resource_type::dns_zone, zone, sync::marker_operation::sync, 1);
    require(previous.front()[0].value().value() == id, "manual re-initiation resets the same task id");
    const auto original = co_await sync::claim_marker(first, sync::marker_resource_type::dns_zone);
    require(original.has_value() && original->failures_ == 0, "manual re-initiation clears the current failure count");
    const auto original_lease = lease_of(first, *original);
    const auto retried = co_await finish(first, original_lease, sync::attempt_result::retry("temporary network failure"));
    const auto retry = co_await db.query(
        "SELECT NOT task.is_done AND NOT task.is_ok, task.next_attempt_at > NOW() + INTERVAL '10 seconds', "
        "event.outcome = 'failed' FROM sys_sync_task task JOIN sys_sync_event event ON event.task_id = task.id WHERE event.id = $1",
        *retried.event_id());
    require(retry.front()[0].as<bool>().value() && retry.front()[1].as<bool>().value() && retry.front()[2].as<bool>().value(), "retryable failure schedules the same task and records the failed outcome");
    require(!(co_await sync::claim_marker(second, sync::marker_resource_type::dns_zone)), "retry cannot run before it is due");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 second' WHERE id = $1", id);
    const auto reclaimed = co_await sync::claim_marker(second, sync::marker_resource_type::dns_zone);
    require(reclaimed.has_value() && reclaimed->id_ == id, "another invocation claims a due retry with its own lease");
    const auto before = co_await event_count(db);
    require((co_await finish(first, original_lease, sync::attempt_result::failed("late owner"))).kind() == sync::result_kind::stale, "a former owner cannot finish the new lease");
    const auto current_lease = lease_of(second, *reclaimed);
    auto wrong_version = current_lease;
    wrong_version.marker_.version_ += 1;
    require((co_await finish(second, wrong_version, sync::attempt_result::completed())).kind() == sync::result_kind::stale, "another version cannot finish the task");
    (void)co_await db.execute("UPDATE sys_sync_task SET lease_until = NOW() - INTERVAL '1 second' WHERE id = $1", id);
    require(!co_await sync::renew_running_lease(db, current_lease), "expired leases cannot be resurrected by renewal");
    require((co_await finish(second, current_lease, sync::attempt_result::completed())).kind() == sync::result_kind::stale, "an expired lease cannot commit a result");
    require(co_await event_count(db) == before, "rejected owners and versions create no history");
    {
        auto locked = co_await db.begin_transaction();
        (void)co_await locked.query("SELECT id FROM sys_sync_task WHERE id = $1 FOR UPDATE", id);
        co_await sync::recover_stale_running(first, sync::marker_resource_type::dns_zone);
        require(co_await event_count(db) == before, "recovery skips an expired lease locked by an unfinished domain transaction");
        co_await locked.rollback();
    }
    co_await sync::recover_stale_running(first, sync::marker_resource_type::dns_zone);
    require(co_await event_count(db) == before + 1, "lease recovery records its failed attempt exactly once");
    const auto timed_out = co_await db.query(
        "SELECT outcome, error, started_at IS NULL, duration_ms FROM sys_sync_event "
        "WHERE task_id = $1 ORDER BY id DESC LIMIT 1",
        id);
    require(timed_out.front()[0].value() == "failed" &&
                timed_out.front()[1].value() == sync::lease_recovery_retry_error &&
                timed_out.front()[2].as<bool>().value() && timed_out.front()[3].as<std::int64_t>() == 0,
        "recovery records the timeout without inventing an unknown start time");
    co_await sync::recover_stale_running(second, sync::marker_resource_type::dns_zone);
    require(co_await event_count(db) == before + 1, "another recovery does not duplicate the failed attempt");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 second' WHERE id = $1", id);
    const auto recovered = co_await sync::claim_marker(first, sync::marker_resource_type::dns_zone);
    require(recovered.has_value(), "the expired lease is recovered by the ordinary claim path");
    const auto completed = co_await finish(first, lease_of(first, *recovered), sync::attempt_result::completed());
    require(completed.kind() == sync::result_kind::recorded, "the recovery owner completes the task");
    const auto rows = co_await db.query("SELECT is_done AND is_ok, processed_version = version, count_fails = 0 FROM sys_sync_task WHERE id = $1", id);
    require(rows.front()[0].as<bool>().value() && rows.front()[1].as<bool>().value() && rows.front()[2].as<bool>().value(), "success finishes the version and clears retry state");
}

ruvia::task<void> verify_waiting(task_context& context) {
    auto db = context.db();
    const auto awaited_id = co_await enqueue(context, sync::marker_resource_type::dns_zone, zone, sync::marker_operation::sync, 1);
    (void)co_await enqueue(context, sync::marker_resource_type::website, website, sync::marker_operation::apply, 1);
    const auto waiter = co_await sync::claim_marker(context, sync::marker_resource_type::website);
    require(waiter.has_value(), "the website starts its attempt");
    const auto waiter_lease = sync::make_running_lease(waiter->tenant_id_, waiter->id_, waiter->version_, context.lease_owner(),
        waiter->claimed_at_, sync::marker_resource_type::website, website);
    const auto parked = co_await finish(context, waiter_lease, sync::attempt_result::waiting(awaited_id, 1));
    const auto event = co_await db.query("SELECT outcome FROM sys_sync_event WHERE id = $1", *parked.event_id());
    require(event.front()[0].value().value() == "progressed", "waiting records the execution outcome");
    require(!(co_await sync::claim_marker(context, sync::marker_resource_type::website)), "a parked task cannot be claimed");
    const auto awaited = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    require(awaited.has_value(), "the dependency can run independently");
    const auto failed = co_await finish(context, lease_of(context, *awaited), sync::attempt_result::failed("dependency permanently failed"));
    require(failed.kind() == sync::result_kind::recorded, "a terminal dependency records its result");
    const auto resumed = co_await db.query("SELECT wait_task_id IS NULL AND wait_version IS NULL, next_attempt_at > NOW() FROM sys_sync_task WHERE id = $1", waiter->id_);
    require(resumed.front()[0].as<bool>().value() && resumed.front()[1].as<bool>().value(), "resumption preserves the bounded settle delay");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 second' WHERE id = $1", waiter->id_);
    const auto ready = co_await sync::claim_marker(context, sync::marker_resource_type::website);
    require(ready.has_value(), "the resumed waiter is claimable");
    const auto ready_lease = sync::make_running_lease(ready->tenant_id_, ready->id_, ready->version_, context.lease_owner(),
        ready->claimed_at_, sync::marker_resource_type::website, website);
    (void)co_await finish(context, ready_lease, sync::attempt_result::completed());
}

ruvia::task<void> verify_rollback(task_context& context) {
    auto db = context.db();
    const auto id = co_await enqueue(context, sync::marker_resource_type::dns_zone, zone, sync::marker_operation::sync, 1);
    const auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    require(marker.has_value(), "rollback fixture is claimed");
    const auto before = co_await event_count(db);
    const auto outbox_before = co_await db.query("SELECT COUNT(*) FROM sys_notification_outbox");
    {
        auto transaction = co_await db.begin_transaction();
        (void)co_await transaction.execute("UPDATE sys_dns_zone SET sync_status = 'failed' WHERE id = $1", zone);
        sync::running_result_writer writer(transaction, lease_of(context, *marker));
        const auto transition = co_await writer.record(
            sync::attempt_result::failed("rolled back"), sync::detail_none_json);
        require(transition.event_id().has_value(), "the uncommitted transaction contains its result event");
        co_await transaction.rollback();
    }
    const auto after = co_await db.query("SELECT NOT is_done, lease_owner = $2, error = '' FROM sys_sync_task WHERE id = $1", id, context.lease_owner());
    require(after.front()[0].as<bool>().value() && after.front()[1].as<bool>().value() && after.front()[2].as<bool>().value(), "rollback restores the task's running lease");
    require(co_await event_count(db) == before, "rollback removes the result event");
    const auto outbox_after = co_await db.query("SELECT COUNT(*) FROM sys_notification_outbox");
    require(outbox_before.front()[0].as<int>().value() == outbox_after.front()[0].as<int>().value(), "rollback publishes no durable notification");
    const auto zone_state = co_await db.query("SELECT sync_status FROM sys_dns_zone WHERE id = $1", zone);
    require(zone_state.front()[0].value().value() == "pending", "rollback also restores the domain result");
    require((co_await finish(context, lease_of(context, *marker), sync::attempt_result::completed())).changed(), "the original owner can still finish after rollback");
}

ruvia::task<void> verify_quiet_and_removal(task_context& context) {
    auto db = context.db();
    const auto awaited_id = co_await enqueue(context, sync::marker_resource_type::dns_zone, zone, sync::marker_operation::sync, 1);
    (void)co_await enqueue(context, sync::marker_resource_type::website, website, sync::marker_operation::apply, 1);
    const auto waiter = co_await sync::claim_marker(context, sync::marker_resource_type::website);
    require(waiter.has_value(), "the removal fixture has a running dependent");
    const auto waiter_lease = sync::make_running_lease(tenant, waiter->id_, 1, context.lease_owner(),
        waiter->claimed_at_, sync::marker_resource_type::website, website);
    // A removal releases even dependents awaiting a version not yet processed.
    (void)co_await finish(context, waiter_lease, sync::attempt_result::waiting(awaited_id, 2));
    const auto claimed = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    require(claimed.has_value(), "the inspected dependency is claimed");
    const auto inspected_lease = lease_of(context, *claimed);
    const auto before = co_await event_count(db);
    test_support::recorded_notifications notifications{.handle_ = db};
    const auto schedule = co_await db.query(
        "SELECT next_attempt_at::text, updated_at::text FROM sys_sync_task WHERE id = $1", awaited_id);
    {
        auto transaction = co_await db.begin_transaction();
        sync::running_result_writer writer(transaction, inspected_lease);
        (void)co_await transaction.execute("UPDATE sys_dns_zone SET sync_status = 'synced' WHERE id = $1", zone);
        const auto result = co_await writer.quiet(sync::quiet_action::inspect);
        require(result.kind() == sync::result_kind::quiet && !result.event_id(), "an unchanged inspection is explicitly quiet");
        co_await writer.commit(notifications);
    }
    const auto inspected = co_await db.query(
        "SELECT lease_owner IS NULL, NOT is_done, next_attempt_at::text, updated_at::text FROM sys_sync_task WHERE id = $1",
        awaited_id);
    require(inspected.front()[0].as<bool>().value() && inspected.front()[1].as<bool>().value() &&
                inspected.front()[2].value() == schedule.front()[0].value() &&
                inspected.front()[3].value() == schedule.front()[1].value(),
        "quiet inspection releases only its lease and preserves the task schedule and state");
    require(co_await event_count(db) == before && notifications.sent_.empty(), "quiet inspection creates neither history nor a notification");
    const auto domain = co_await db.query("SELECT sync_status FROM sys_dns_zone WHERE id = $1", zone);
    require(domain.front()[0].value() == "synced", "the domain transaction still commits its own updates");
    const auto reclaimed = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    require(reclaimed.has_value(), "quiet inspection leaves unfinished work claimable");
    const auto lease = lease_of(context, *reclaimed);
    {
        auto wrong_owner = lease;
        wrong_owner.owner_ = "not-the-owner";
        auto transaction = co_await db.begin_transaction();
        sync::running_result_writer writer(transaction, wrong_owner);
        require((co_await writer.remove()).kind() == sync::result_kind::stale, "a stale deletion cannot remove a running task");
        co_await writer.commit(notifications);
    }
    {
        auto transaction = co_await db.begin_transaction();
        sync::running_result_writer writer(transaction, lease);
        const auto result = co_await writer.remove();
        require(result.kind() == sync::result_kind::removed && !result.event_id(), "removal has its own receipt, not a fabricated event");
        const auto resumed = co_await transaction.query(
            "SELECT wait_task_id IS NULL AND wait_version IS NULL FROM sys_sync_task WHERE id = $1", waiter->id_);
        require(resumed.front()[0].as<bool>().value(), "removal releases future-version dependents in the same transaction");
        co_await transaction.rollback();
    }
    const auto rolled_back = co_await db.query(
        "SELECT wait_task_id = $2 AND wait_version = 2 FROM sys_sync_task WHERE id = $1", waiter->id_, awaited_id);
    require(rolled_back.front()[0].as<bool>().value(), "rollback restores the dependency wait");
    const auto restored = co_await db.query("SELECT lease_owner = $2 FROM sys_sync_task WHERE id = $1", awaited_id, context.lease_owner());
    require(restored.size() == 1 && restored.front()[0].as<bool>().value(), "rollback restores the removed task and its running lease");
    require(co_await event_count(db) == before && notifications.sent_.empty(), "stale and rolled-back removals publish nothing");
    {
        auto transaction = co_await db.begin_transaction();
        sync::running_result_writer writer(transaction, lease);
        require((co_await writer.remove()).kind() == sync::result_kind::removed, "the current owner removes the task");
        co_await writer.commit(notifications);
    }
    require((co_await db.query("SELECT id FROM sys_sync_task WHERE id = $1", awaited_id)).empty(), "committed removal deletes the task");
    const auto resumed = co_await db.query(
        "SELECT wait_task_id IS NULL AND wait_version IS NULL, next_attempt_at > NOW() FROM sys_sync_task WHERE id = $1", waiter->id_);
    require(resumed.front()[0].as<bool>().value() && resumed.front()[1].as<bool>().value(), "removal resumes dependents with their settle delay");
    require(co_await event_count(db) == before && notifications.sent_.size() == 1 &&
                notifications.committed_.front() && notifications.sent_.front().task_attempts_.empty(),
        "removal publishes only its committed changes and creates no attempt history");
    const auto& changed = notifications.sent_.front().changes_;
    require(std::ranges::any_of(changed, [](const auto& item) {
                return item.resource_ == service::live_resource::resource::tasks && item.id_ == zone;
            }) && std::ranges::any_of(changed, [](const auto& item) {
                return item.resource_ == service::live_resource::resource::tasks && item.id_ == website;
            }),
        "the removal notification includes the deleted task and its resumed dependent");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 second' WHERE id = $1", waiter->id_);
    const auto ready = co_await sync::claim_marker(context, sync::marker_resource_type::website);
    require(ready.has_value(), "the resumed dependent runs normally");
    const auto ready_lease = sync::make_running_lease(tenant, ready->id_, 1, context.lease_owner(),
        ready->claimed_at_, sync::marker_resource_type::website, website);
    {
        auto transaction = co_await db.begin_transaction();
        sync::running_result_writer writer(transaction, ready_lease);
        require((co_await writer.quiet(sync::quiet_action::complete)).kind() == sync::result_kind::quiet, "obsolete work can complete quietly");
        co_await writer.commit(notifications);
    }
    const auto completed = co_await db.query("SELECT is_done AND is_ok, processed_version = version FROM sys_sync_task WHERE id = $1", ready->id_);
    require(completed.front()[0].as<bool>().value() && completed.front()[1].as<bool>().value() &&
                co_await event_count(db) == before && notifications.sent_.size() == 1,
        "quiet completion finishes the version without history or duplicate notifications");
}

ruvia::task<bool> wait_for_resource_lock(task_context& context, std::int64_t holder) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto blocked = co_await context.db().query(
            "SELECT COUNT(*) FROM pg_stat_activity WHERE datname = current_database() AND "
            "wait_event_type = 'Lock' AND $1 = ANY(pg_blocking_pids(pid))",
            holder);
        if (blocked.front()[0].as<std::int64_t>().value() != 0) {
            co_return true;
        }
        (void)co_await ruvia::sleep_for(context.worker(), std::chrono::milliseconds(10), context.get_stop_token());
    }
    co_return false;
}

// A second transaction holds the aggregate while the maintenance scan reads
// its missing/outdated candidate. pg_blocking_pids confirms maintenance has
// reached FOR UPDATE before that holder commits a terminal result. This is
// the actual production interleaving, without hooks or timing assumptions.
ruvia::task<void> verify_stale_maintenance_candidates(ruvia::web_worker_context& web, task_context& context) {
    struct repair_case final {
        sync::marker_resource_type type_{};
        std::string_view id_{};
        sync::marker_operation operation_{};
        std::string_view lock_sql_{};
        std::int64_t version_{};
    };
    const repair_case cases[]{
        {.type_ = sync::marker_resource_type::dns_zone, .id_ = zone, .operation_ = sync::marker_operation::sync, .lock_sql_ = "SELECT id FROM sys_dns_zone WHERE id = $1 FOR UPDATE", .version_ = 1},
        {.type_ = sync::marker_resource_type::certificate, .id_ = certificate, .operation_ = sync::marker_operation::renew, .lock_sql_ = "SELECT id FROM sys_certificate WHERE id = $1 FOR UPDATE", .version_ = 2},
        {.type_ = sync::marker_resource_type::provider, .id_ = provider, .operation_ = sync::marker_operation::verify, .lock_sql_ = "SELECT id FROM sys_provider WHERE id = $1 FOR UPDATE", .version_ = 2},
        {.type_ = sync::marker_resource_type::website, .id_ = website, .operation_ = sync::marker_operation::apply, .lock_sql_ = "SELECT id FROM sys_website WHERE id = $1 FOR UPDATE", .version_ = 1}};
    auto db = context.db();
    for (const auto& repair : cases) {
        if (repair.type_ == sync::marker_resource_type::provider) {
            (void)co_await db.execute("UPDATE sys_provider SET verification_generation = 2 WHERE id = $1", repair.id_);
        } else {
            (void)co_await db.execute("DELETE FROM sys_sync_task WHERE tenant_id = $1 AND resource_id = $2", tenant, repair.id_);
        }
        auto holder = co_await db.begin_transaction();
        (void)co_await holder.query(repair.lock_sql_, repair.id_);
        const auto identity = co_await holder.query("SELECT pg_backend_pid()");
        const auto before = co_await event_count(db);
        const auto outbox_before = co_await db.query("SELECT COUNT(*) FROM sys_notification_outbox");
        ruvia::task_scope tasks(context.worker(), {.resource_ = context.pool()});
        task_context repair_context(web, tasks.get_stop_token());
        std::exception_ptr failure;
        auto restore = [&]() -> ruvia::task<void> {
            try {
                switch (repair.type_) {
                    case sync::marker_resource_type::dns_zone:
                        co_await service::dns_sync::detail::reconcile_tasks(repair_context);
                        break;
                    case sync::marker_resource_type::certificate:
                        co_await service::certificate_issuance::worker_detail::reconcile_missing_issuance_markers(repair_context);
                        break;
                    case sync::marker_resource_type::provider:
                        co_await service::provider_verification::detail::reconcile(repair_context);
                        break;
                    case sync::marker_resource_type::website:
                        co_await service::website_dns::detail::reconcile_markers(repair_context);
                        break;
                }
            } catch (...) {
                failure = std::current_exception();
            }
        };
        tasks.spawn(restore());
        const auto blocked = repair.type_ == sync::marker_resource_type::dns_zone ||
                             co_await wait_for_resource_lock(context, identity.front()[0].as<std::int64_t>().value());
        if (repair.type_ == sync::marker_resource_type::dns_zone) {
            // DNS recovery skips locked resources and retries them on its next scan.
            co_await tasks.join();
        }
        if (!blocked) {
            co_await holder.rollback();
            tasks.request_stop();
            co_await tasks.join();
            if (failure) {
                std::rethrow_exception(failure);
            }
            require(false, "maintenance must select its candidate and block on the resource lock");
        }
        service::background::queued_work work;
        const auto id = co_await sync::upsert_marker(holder, tenant, repair.type_, repair.id_, repair.operation_, repair.version_, work);
        (void)co_await holder.execute("UPDATE sys_sync_task SET lease_owner = $2, lease_until = NOW() + INTERVAL '60 seconds' WHERE id = $1",
            id, context.lease_owner());
        const auto lease = sync::make_running_lease(tenant, id, repair.version_, context.lease_owner(),
            std::chrono::steady_clock::now(), repair.type_, repair.id_);
        sync::running_result_writer writer(holder, lease);
        (void)co_await writer.record(sync::attempt_result::failed("finished during maintenance scan"), sync::detail_none_json);
        service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web", lease.started_);
        co_await writer.commit(context, trace);
        if (repair.type_ != sync::marker_resource_type::dns_zone) {
            co_await tasks.join();
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
        const auto current = co_await db.query("SELECT id::text = $2, is_done AND NOT is_ok, count_fails, error FROM sys_sync_task WHERE tenant_id = $1 AND resource_id = $3",
            tenant, std::string_view(id), repair.id_);
        require(current.size() == 1 && current.front()[0].as<bool>().value() && current.front()[1].as<bool>().value() &&
                    current.front()[2].as<int>().value() == 1 && current.front()[3].value().value() == "finished during maintenance scan",
            "a stale maintenance candidate preserves the concurrently committed terminal task");
        require(co_await event_count(db) == before + 1, "maintenance adds no result after the concurrent terminal event");
        const auto outbox_after = co_await db.query("SELECT COUNT(*) FROM sys_notification_outbox");
        require(outbox_after.front()[0].as<std::int64_t>().value() == outbox_before.front()[0].as<std::int64_t>().value() + 1,
            "maintenance emits no reset notification for a preserved result");
        require(!(co_await sync::claim_marker(context, repair.type_)), "the preserved result remains terminal");
    }
}

ruvia::task<void> verify_missing_domain_markers(task_context& context) {
    constexpr std::string_view recovery_tenant{"00000000-0000-4000-8000-000000000b01"};
    auto db = context.db();
    (void)co_await db.execute(
        "INSERT INTO sys_certificate(id, tenant_id, issuance_revision, issued_revision, deleted_at) VALUES "
        "('00000000-0000-4000-8000-000000000b02', $1, 1, 0, NULL), "
        "('00000000-0000-4000-8000-000000000b03', $1, 3, 2, NULL), "
        "('00000000-0000-4000-8000-000000000b04', $1, 2, 2, NULL), "
        "('00000000-0000-4000-8000-000000000b05', $1, 1, 0, NOW())",
        recovery_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, verification_generation, deleted_at) VALUES "
        "('00000000-0000-4000-8000-000000000b06', $1, 3, NULL), "
        "('00000000-0000-4000-8000-000000000b07', $1, 3, NOW())",
        recovery_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_website(id, tenant_id, revision, deleted_at) VALUES "
        "('00000000-0000-4000-8000-000000000b08', $1, 4, NULL), "
        "('00000000-0000-4000-8000-000000000b09', $1, 4, NOW())",
        recovery_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, desired_revision, synced_revision, deleted_at) VALUES "
        "('00000000-0000-4000-8000-000000000b0a', $1, 3, 2, NOW()), "
        "('00000000-0000-4000-8000-000000000b0b', $1, 3, 3, NOW())",
        recovery_tenant);
    for (int round = 0; round < 2; ++round) {
        co_await service::certificate_issuance::worker_detail::reconcile_missing_issuance_markers(context);
        co_await service::provider_verification::detail::reconcile(context);
        co_await service::website_dns::detail::reconcile_markers(context);
        co_await service::dns_sync::detail::reconcile_tasks(context);
        const auto restored = co_await db.query(
            "SELECT COUNT(*), "
            "COUNT(*) FILTER (WHERE resource_type = 'certificate' AND operation = 'issue' AND version = 1), "
            "COUNT(*) FILTER (WHERE resource_type = 'certificate' AND operation = 'renew' AND version = 3), "
            "COUNT(*) FILTER (WHERE resource_type = 'provider' AND operation = 'verify' AND version = 3), "
            "COUNT(*) FILTER (WHERE resource_type = 'website' AND operation = 'apply' AND version = 4), "
            "COUNT(*) FILTER (WHERE resource_type = 'dns_zone' AND operation = 'delete' AND version = 3) "
            "FROM sys_sync_task WHERE tenant_id = $1",
            recovery_tenant);
        require(restored.front()[0].as<int>().value() == 5,
            "domain recovery excludes settled certificates, removed providers/websites and completed DNS deletions");
        for (std::size_t column = 1; column < 6; ++column) {
            require(restored.front()[column].as<int>().value() == 1,
                "each domain restores its own operation and current revision exactly once");
        }
        if (round == 0) {
            (void)co_await db.execute(
                "UPDATE sys_sync_task SET is_done = TRUE, is_ok = FALSE, processed_version = version, "
                "count_fails = 2, error = 'retained domain failure' WHERE tenant_id = $1",
                recovery_tenant);
        }
    }
    const auto retained = co_await db.query(
        "SELECT COUNT(*) FROM sys_sync_task WHERE tenant_id = $1 AND is_done AND NOT is_ok "
        "AND count_fails = 2 AND error = 'retained domain failure'",
        recovery_tenant);
    require(retained.front()[0].as<int>().value() == 5, "repeated domain recovery preserves terminal failures");
}

ruvia::task<void> verify_recovery_skips_locked_batch(task_context& context) {
    constexpr std::string_view recovery_tenant{"00000000-0000-4000-8000-000000000d01"};
    auto db = context.db();
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task (id, tenant_id, resource_type, dns_zone_id, operation, version, "
        "lease_owner, lease_until) SELECT md5('locked-recovery-task-' || sequence::text)::uuid, $1, "
        "'dns_zone', md5('locked-recovery-zone-' || sequence::text)::uuid, 'sync', 1, $2, "
        "NOW() - make_interval(secs => 100 - sequence) FROM generate_series(1, 33) AS candidate(sequence)",
        recovery_tenant, context.lease_owner());
    const auto before = co_await event_count(db);
    auto locked = co_await db.begin_transaction();
    const auto held = co_await locked.query(
        "SELECT id FROM sys_sync_task WHERE tenant_id = $1 ORDER BY lease_until, id LIMIT 32 FOR UPDATE",
        recovery_tenant);
    require(held.size() == 32, "the oldest full recovery batch is locked by another transaction");
    co_await sync::recover_stale_running(context, sync::marker_resource_type::dns_zone);
    const auto recovered = co_await db.query(
        "SELECT task.id = md5('locked-recovery-task-33')::uuid, task.lease_owner IS NULL, "
        "event.outcome = 'failed' FROM sys_sync_task task JOIN sys_sync_event event ON event.task_id = task.id "
        "WHERE task.tenant_id = $1",
        recovery_tenant);
    require(recovered.size() == 1 && recovered.front()[0].as<bool>().value() &&
                recovered.front()[1].as<bool>().value() && recovered.front()[2].as<bool>().value(),
        "one recovery round reaches the unlocked expired lease behind the 32 locked candidates");
    const auto untouched = co_await db.query(
        "SELECT COUNT(*) FROM sys_sync_task task WHERE tenant_id = $1 AND lease_owner = $2 "
        "AND lease_until <= NOW() AND count_fails = 0 AND NOT EXISTS "
        "(SELECT 1 FROM sys_sync_event event WHERE event.task_id = task.id)",
        recovery_tenant, context.lease_owner());
    require(untouched.front()[0].as<int>().value() == 32 && co_await event_count(db) == before + 1,
        "locked candidates retain their leases and create no recovery history");
    co_await locked.rollback();
    co_await sync::recover_stale_running(context, sync::marker_resource_type::dns_zone);
    const auto resumed = co_await db.query(
        "SELECT COUNT(*), COUNT(*) FILTER (WHERE lease_owner IS NULL AND count_fails = 1) "
        "FROM sys_sync_task WHERE tenant_id = $1",
        recovery_tenant);
    require(resumed.front()[0].as<int>().value() == 33 && resumed.front()[1].as<int>().value() == 33 &&
                co_await event_count(db) == before + 33,
        "the released batch is recovered normally without duplicating the already recovered lease");
    co_await sync::recover_stale_running(context, sync::marker_resource_type::dns_zone);
    require(co_await event_count(db) == before + 33, "later scans do not duplicate either recovery batch");
}

ruvia::task<void> verify(ruvia::web_worker_context& web) {
    task_context first(web);
    task_context second(web);
    co_await prepare(first.db());
    co_await verify_terminal_failures(first);
    co_await verify_retry_and_fences(first, second);
    co_await verify_waiting(first);
    co_await verify_rollback(first);
    co_await verify_quiet_and_removal(first);
    co_await verify_stale_maintenance_candidates(web, first);
    co_await verify_missing_domain_markers(first);
    co_await verify_recovery_skips_locked_batch(first);
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_postgres_config();
        if (!config) {
            return 77;
        }
        const test_support::postgres_test_database database(*config);
        asio::io_context io;
        asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto port = acceptor.local_endpoint().port();
        acceptor.close();
        auto& app = ruvia::app();
        auto worker_database = database.config();
        worker_database.pool_size_per_worker_ = 3;
        app.database({.config_ = worker_database});
        app.server({.worker_count_ = 1});
        app.listen({.address_ = "127.0.0.1", .http_ = port});
        app.use_worker_state<service::background::work_available::claim_signals>();
        std::exception_ptr failure;
        bool completed{};
        app.on_start([&] {
            for (const auto& worker : app.workers()) {
                require(worker.post([&](ruvia::web_worker_context& context) -> ruvia::task<void> {
                                  try {
                                      co_await verify(context);
                                      completed = true;
                                  } catch (...) {
                                      failure = std::current_exception();
                                  }
                                  app.stop();
                              })
                            .accepted(),
                    "the verification task is accepted");
            }
        });
        app.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        require(completed, "the real Web Worker completes its owned operations before shutdown");
        std::cout << "Sync outcomes: terminal failures, bounded retries, domain recovery eligibility and operations, re-initiation, leases, dependencies, quiet inspection, atomic removal/history/notification rollback and four controlled maintenance races passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
