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
#include "service/features/background/queued_work.h"
#include "service/features/dns_sync/attempt_detail.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/live_resource/notifications.h"
#include "service/features/node_dispatch/apply_history.h"
#include "service/features/node_dispatch/release_store.h"
#include "service/features/node_dispatch/target_store.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"
#include "service/features/sync_runtime/worker.h"
#include "service/features/website_dns/attempt_detail.h"
#include "service/utils/secret.h"
#include "support/postgres_test.h"
#include "support/read_context.h"
#include "support/recorded_notifications.h"

// The task center on the real baseline: GoEdge tasks, one task per resource
// that a re-initiation resets in place,
// its status and warnings, node tasks from release targets and node specs,
// and the resource's attempts as the task's history.
namespace {

using service::live_resource::resource;
using service::sync_runtime::marker_operation;
using service::sync_runtime::marker_resource_type;
using read_context = test_support::read_context<ruvia::db_handle>;
using recorder = test_support::recorded_notifications;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("task center: " + std::string(message));
    }
}

constexpr std::string_view worker_name = "task-center-qa";

constexpr std::string_view tenant = "00000000-0000-4000-8000-0000000004a1";
constexpr std::string_view other_tenant = "00000000-0000-4000-8000-0000000004a2";
constexpr std::string_view dns_provider = "00000000-0000-4000-8000-0000000004b1";
constexpr std::string_view cluster_zone = "00000000-0000-4000-8000-0000000004c0";
constexpr std::string_view cluster = "00000000-0000-4000-8000-0000000004e1";
constexpr std::string_view website = "00000000-0000-4000-8000-0000000004f1";
constexpr std::string_view node_online = "00000000-0000-4000-8000-000000000491";
constexpr std::string_view node_offline = "00000000-0000-4000-8000-000000000492";
constexpr std::string_view old_release = "00000000-0000-4000-8000-000000000481";
constexpr std::string_view current_release = "00000000-0000-4000-8000-000000000482";
constexpr std::string_view synced_marker = "00000000-0000-4000-8000-000000000471";
constexpr std::string_view failing_marker = "00000000-0000-4000-8000-000000000472";

// Zone `index` (1-9) of the tenant: z<index>.example.com.
std::string zone(int index) {
    return "00000000-0000-4000-8000-00000000040" + std::to_string(index);
}

std::string zone_domain(int index) {
    return "z" + std::to_string(index) + ".example.com";
}

int migrate(const test_support::postgres_test_database& config, std::span<const ruvia::db_migration> migrations,
    std::size_t expected_applied, std::string_view label) {
    const ruvia::db_migrator migrator(config.config(), {.table_ = "schema_migrations"});
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

// ---------------------------------------------------------------------------
// Tasks

ruvia::task<void> insert_fixture(ruvia::db_handle& db) {
    (void)co_await db.execute(
        "INSERT INTO sys_tenant(id, name, slug) VALUES ($1, 'Task QA', 'task-qa'), ($2, 'Task other', 'task-other')",
        tenant, other_tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_provider(id, tenant_id, kind, provider, name, account_id, status) VALUES ($1, $2, 'dns', "
        "'cloudflare', 'Task DNS', 'acct', 'verified')",
        dns_provider, tenant);
    // The schema's empty JSON default is not a valid DNS runtime.
    service::dns_sync::zone_runtime_data runtime{};

    const auto runtime_json = ruvia::to_json(service::dns_sync::to_output(runtime));
    (void)co_await db.execute(
        "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, sync_status, synced_revision, runtime) VALUES "
        "($1, $2, $3, 'edge.example.com', 'synced', 1, $4::jsonb)",
        cluster_zone, tenant, dns_provider, std::string_view(runtime_json));
    for (int index = 1; index <= 5; ++index) {
        (void)co_await db.execute(
            "INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain, sync_status, synced_revision, runtime) "
            "VALUES ($1, $2, $3, $4, 'synced', 1, $5::jsonb)",
            zone(index), tenant, dns_provider, zone_domain(index), std::string_view(runtime_json));
    }
    (void)co_await db.execute(
        "INSERT INTO sys_cluster(id, tenant_id, dns_zone_id, name, hostname_prefix) VALUES ($1, $2, $3, 'Edge', "
        "'edge')",
        cluster, tenant, cluster_zone);
    (void)co_await db.execute("INSERT INTO sys_website(id, tenant_id, cluster_id, config) VALUES ($1, $2, $3, '{}')",
        website, tenant, cluster);
    // Both nodes run their current node spec; only the first one is online.
    (void)co_await db.execute(
        "INSERT INTO sys_node(id, tenant_id, cluster_id, name, registration_status, registered_at, "
        "applied_node_spec_revision, agent_id, node_secret_hash, node_secret_envelope, last_heartbeat_at) VALUES "
        "($1, $3, $4, '北京', 'registered', NOW(), 1, repeat('a', 32), repeat('c', 64), 'qa-only', NOW()), ($2, $3, "
        "$4, '上海', 'registered', NOW(), 1, repeat('b', 32), repeat('d', 64), 'qa-only', NULL)",
        node_online, node_offline, tenant, cluster);
}

ruvia::task<service::task::task_dto> task_of(read_context& context, std::string_view type, std::string_view id) {
    co_return co_await service::task::get_task_service().detail(context, std::string(tenant), std::string(type),
        std::string(id));
}

ruvia::task<std::string> status_of(read_context& context, std::string_view type, std::string_view id) {
    const auto task = co_await task_of(context, type, id);
    co_return std::string(task.get<"status">().view());
}

ruvia::task<bool> task_missing(read_context& context, std::string_view tenant_id, std::string_view type,
    std::string_view id) {
    try {
        (void)co_await service::task::get_task_service().detail(context, std::string(tenant_id), std::string(type),
            std::string(id));
    } catch (const std::exception&) {
        co_return true;
    }
    co_return false;
}

ruvia::task<service::task::task_page_dto> list(read_context& context, std::string status, std::string type,
    std::string keyword = {}) {
    co_return co_await service::task::get_task_service().list(context, std::string(tenant), 1, 100, 0,
        service::task::list_filter{.status_ = std::move(status), .type_ = std::move(type), .keyword_ = std::move(keyword)});
}

bool listed(const service::task::task_page_dto& page, std::string_view id) {
    const auto& items = page.get<"list">();
    return std::any_of(items.begin(), items.end(),
        [&](const service::task::task_dto& item) { return item.get<"resource_id">().view() == id; });
}

ruvia::task<std::int64_t> total_of(read_context& context, std::string status, std::string type,
    std::string keyword = {}) {
    const auto page = co_await list(context, std::move(status), std::move(type), std::move(keyword));
    co_return page.get<"total">().value_;
}

ruvia::task<service::task::task_history_dto> history_of(read_context& context, std::string_view type,
    std::string_view id) {
    co_return co_await service::task::get_task_service().history(context, std::string(tenant), std::string(type),
        std::string(id));
}

bool changes_task(const service::live_resource::notifications::resource_notification& notification,
    std::string_view id) {
    return std::any_of(notification.changes_.begin(), notification.changes_.end(),
        [&](const auto& change) { return change.resource_ == resource::tasks && change.id_ == id; });
}

// (Re-)initiates the resource's task the way every write does: upsert the
// marker, then commit the queued work.
ruvia::task<void> initiate(ruvia::db_handle& db, recorder& worker, marker_resource_type type, std::string_view id,
    marker_operation operation, std::int64_t version) {
    service::background::queued_work work;
    auto transaction = co_await db.begin_transaction();
    (void)co_await service::sync_runtime::upsert_marker(transaction, tenant, type, id, operation, version, work);
    co_await service::sync_runtime::commit_work(transaction, worker, tenant, work);
}

service::attempt_trace::trace_kind trace_kind_of(marker_resource_type type) {
    switch (type) {
        case marker_resource_type::dns_zone:
            return service::attempt_trace::trace_kind::dns_sync;
        case marker_resource_type::certificate:
            return service::attempt_trace::trace_kind::certificate;
        case marker_resource_type::website:
            return service::attempt_trace::trace_kind::website_domains;
        case marker_resource_type::provider:
            return service::attempt_trace::trace_kind::provider;
    }
    throw std::invalid_argument("unsupported marker type");
}

// Claims the resource's marker at its current version like a worker, then
// records one attempt through the production result transition.
ruvia::task<service::sync_runtime::running_result_transition> run(ruvia::db_handle& db, recorder& worker,
    marker_resource_type type, std::string_view id, bool ok,
    std::string_view detail = service::sync_runtime::detail_none_json, std::string_view error = "QA 同步失败",
    bool terminal = false) {
    const auto marker = co_await db.query(
        "UPDATE sys_sync_task SET lease_owner = $4, lease_until = NOW() + INTERVAL '60 seconds' WHERE tenant_id = $1 "
        "AND resource_type = $2 AND resource_id = $3 RETURNING id::text, version",
        tenant, service::sync_runtime::describe(type).name_, id, worker_name);
    require(!marker.empty(), "the resource has a marker: " + std::string(id));
    const auto marker_id = text(marker);
    const auto lease = service::sync_runtime::make_running_lease(tenant, marker_id, integer(marker, 1), worker_name,
        std::chrono::steady_clock::now() - std::chrono::milliseconds(20), type, id);
    auto transaction = co_await db.begin_transaction();
    const auto transition = co_await service::sync_runtime::record_running_result(transaction, lease,
        ok         ? service::sync_runtime::attempt_result::completed()
        : terminal ? service::sync_runtime::attempt_result::failed(error)
                   : service::sync_runtime::attempt_result::retry(error),
        detail);
    service::attempt_trace::recorder trace(trace_kind_of(type), service::sync_runtime::describe(type).work_type_,
        std::chrono::steady_clock::now());
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, worker, lease, transition, trace);
    require(transition.marker_transitioned_ && transition.event_id_.has_value(), "attempt recorded");
    co_return transition;
}

// (b) A failing marker task retried to success stays one task; (c) its
// re-initiation resets the same task; (e) its history spans both versions.
ruvia::task<void> verify_marker_task(ruvia::db_handle& db, read_context& context) {
    recorder worker{.handle_ = db};
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(1), marker_operation::sync, 2);
    require(worker.own_claims_ == std::vector<std::string>{"dns-sync"} && worker.sent_.size() == 1 &&
                worker.committed_[0] && changes_task(worker.sent_[0], zone(1)),
        "the write notifies the zone's task after its commit and its worker claims dns-sync first");
    const auto queued = co_await task_of(context, "dns_zone", zone(1));
    require(queued.get<"status">().view() == "queued" && queued.get<"operation">().view() == "sync" &&
                queued.get<"name">().view() == zone_domain(1) && queued.get<"resource_kind">().view().empty() &&
                queued.get<"attempt_count">().value_ == 0 && queued.get<"failure_count">().value_ == 0,
        "a new task is queued");

    (void)co_await db.execute(
        "UPDATE sys_sync_task SET lease_owner = $3, lease_until = NOW() + INTERVAL '60 seconds' WHERE tenant_id = $1 "
        "AND dns_zone_id = $2",
        tenant, zone(1), worker_name);
    require(co_await status_of(context, "dns_zone", zone(1)) == "running",
        "a leased marker is running");

    const auto failed = co_await run(db, worker, marker_resource_type::dns_zone, zone(1), false);
    const auto& failure_sent = worker.sent_.back();
    require(worker.committed_.back() && failure_sent.task_attempts_.size() == 1 &&
                failure_sent.task_attempts_[0].task_id_ == zone(1) &&
                failure_sent.task_attempts_[0].notice_.event_id_ == *failed.event_id_ &&
                changes_task(failure_sent, zone(1)),
        "the result notifies task_attempt(zone, event) after its commit");
    const auto retrying = co_await task_of(context, "dns_zone", zone(1));
    require(retrying.get<"status">().view() == "retrying" && retrying.get<"error">().view() == "QA 同步失败" &&
                !retrying.get<"next_attempt_at">().view().empty() && retrying.get<"failure_count">().value_ == 1 &&
                retrying.get<"attempt_count">().value_ == 1 && retrying.get<"last_success_at">().view().empty(),
        "a failed attempt schedules a retry of the same task");

    const auto succeeded = co_await run(db, worker, marker_resource_type::dns_zone, zone(1), true);
    const auto completed = co_await task_of(context, "dns_zone", zone(1));
    require(completed.get<"status">().view() == "completed" && completed.get<"error">().view().empty() &&
                completed.get<"next_attempt_at">().view().empty() &&
                !completed.get<"last_success_at">().view().empty() && completed.get<"failure_count">().value_ == 0 &&
                completed.get<"attempt_count">().value_ == 2,
        "the retry completes the same task: 失败 1 次后成功");
    const auto one = co_await list(context, "", "dns_zone", zone(1));
    require(one.get<"total">().value_ == 1 && one.get<"list">().size() == 1 &&
                one.get<"list">()[0].get<"status">().view() == "completed",
        "one task per resource");

    // (c) Re-initiation resets the same task.
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(1), marker_operation::sync, 3);
    const auto reset = co_await task_of(context, "dns_zone", zone(1));
    require(reset.get<"status">().view() == "queued" && reset.get<"operation">().view() == "sync" &&
                reset.get<"failure_count">().value_ == 0 && reset.get<"attempt_count">().value_ == 2 &&
                reset.get<"error">().view().empty(),
        "re-initiation queues the same task again with a fresh failure count and its attempts kept");
    require(co_await total_of(context, "", "dns_zone", zone(1)) == 1,
        "re-initiation creates no second task");
    const auto latest = co_await run(db, worker, marker_resource_type::dns_zone, zone(1), true);
    const auto again = co_await task_of(context, "dns_zone", zone(1));
    require(again.get<"status">().view() == "completed" && again.get<"failure_count">().value_ == 0 &&
                again.get<"attempt_count">().value_ == 3,
        "the re-initiated task completes");

    // (e) The history spans both versions, newest first.
    const auto history = co_await history_of(context, "dns_zone", zone(1));
    const auto& attempts = history.get<"list">();
    require(attempts.size() == 3, "every attempt of the resource");
    require(attempts[0].get<"id">().view() == std::to_string(*latest.event_id_) &&
                attempts[1].get<"id">().view() == std::to_string(*succeeded.event_id_) &&
                attempts[2].get<"id">().view() == std::to_string(*failed.event_id_),
        "newest first, identified by event id");
    require(attempts[0].get<"operation">().view() == "sync" && attempts[0].get<"outcome">().view() == "completed" &&
                attempts[1].get<"operation">().view() == "sync" && attempts[1].get<"outcome">().view() == "completed" &&
                attempts[2].get<"operation">().view() == "sync" && attempts[2].get<"outcome">().view() == "failed" &&
                attempts[2].get<"error">().view() == "QA 同步失败",
        "each attempt keeps its operation and outcome");
    require(history.get<"summary">().get<"attempt_count">().value_ == 3 &&
                !history.get<"summary">().get<"last_success_at">().view().empty(),
        "history summary");

    // Tenants, unknown resources and removed markers have no task.
    require(co_await task_missing(context, other_tenant, "dns_zone", zone(1)), "tasks are tenant scoped");
    require(co_await task_missing(context, tenant, "dns_zone", zone(5)), "a resource without work has no task");
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(4), marker_operation::remove, 2);
    {
        service::background::queued_work work;
        auto transaction = co_await db.begin_transaction();
        co_await service::sync_runtime::remove_marker(transaction, tenant, marker_resource_type::dns_zone, zone(4),
            work);
        require(work.changed_tasks() == std::vector<std::string>{zone(4)}, "removal changes the task");
        co_await service::sync_runtime::commit_work(transaction, worker, tenant, work);
    }
    require(co_await task_missing(context, tenant, "dns_zone", zone(4)), "a removed marker's task disappears");
}

ruvia::task<void> verify_terminal_failure(ruvia::db_handle& db, read_context& context) {
    recorder worker{.handle_ = db};
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(5), marker_operation::sync, 2);
    (void)co_await run(db, worker, marker_resource_type::dns_zone, zone(5), false,
        service::sync_runtime::detail_none_json, "永久配置错误", true);
    (void)co_await db.execute("UPDATE sys_sync_task SET count_fails = 3 WHERE dns_zone_id = $1", zone(5));
    const auto failed = co_await task_of(context, "dns_zone", zone(5));
    require(failed.get<"status">().view() == "failed" && failed.get<"error">().view() == "永久配置错误" &&
                failed.get<"next_attempt_at">().view().empty() &&
                failed.get<"attempt_count">().value_ == 1 && failed.get<"failure_count">().value_ == 3,
        "a terminal marker is failed with its error and no retry schedule");
    const auto active = co_await list(context, "active", "dns_zone");
    const auto failures = co_await list(context, "failed", "dns_zone");
    const auto attention = co_await list(context, "attention", "dns_zone");
    require(!listed(active, zone(5)) && listed(failures, zone(5)) && listed(attention, zone(5)) &&
                failures.get<"failed">().value_ >= 1,
        "terminal failure participates in failed totals and attention, not active tasks");
    const auto history = co_await history_of(context, "dns_zone", zone(5));
    require(history.get<"list">().size() == 1 && history.get<"list">()[0].get<"outcome">().view() == "failed" &&
                history.get<"summary">().get<"failure_count">().value_ == 1,
        "terminal failure retains a failed attempt without a retry time");
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(5), marker_operation::sync, 3);
    const auto restarted = co_await task_of(context, "dns_zone", zone(5));
    require(restarted.get<"status">().view() == "queued" && restarted.get<"error">().view().empty() &&
                restarted.get<"failure_count">().value_ == 0 && restarted.get<"attempt_count">().value_ == 1,
        "re-initiation resets the same terminal task and keeps its history");
}

ruvia::task<void> verify_query_pagination(read_context& context) {
    auto& tasks = service::task::get_task_service();
    const service::task::list_filter filter{};
    const auto first = co_await tasks.list(context, std::string(tenant), 1, 2, 0, filter);
    const auto second = co_await tasks.list(context, std::string(tenant), 2, 2, 2, filter);
    require(first.get<"list">().size() == 2 && second.get<"list">().size() == 2 &&
                first.get<"total">().value_ == second.get<"total">().value_ &&
                first.get<"active">().value_ == second.get<"active">().value_ &&
                first.get<"failed">().value_ == second.get<"failed">().value_,
        "pagination preserves tenant-wide totals and counts");
    for (const auto& task : first.get<"list">()) {
        require(!listed(second, task.get<"resource_id">().view()), "ordered pages do not overlap");
    }
    const auto empty = co_await tasks.list(context, std::string(tenant), 1000, 2, 1998, filter);
    require(empty.get<"list">().empty() && empty.get<"total">().value_ == first.get<"total">().value_ &&
                empty.get<"total_pages">().value_ == first.get<"total_pages">().value_,
        "a page past the end keeps its count");
    const auto keyword = co_await list(context, "", "dns_zone", "Z1.EXAMPLE.COM");
    require(keyword.get<"total">().value_ == 1 && listed(keyword, zone(1)), "keyword matching is case insensitive");
    const auto literal = co_await list(context, "", "", "' OR TRUE --");
    require(literal.get<"total">().value_ == 0, "keyword text remains a bound value");
}

// (f) Completed work that needs attention.
// Task status comes from its own current snapshot, never a previous success's detail.
ruvia::task<void> verify_result_status(ruvia::db_handle& db, read_context& context) {
    recorder worker{.handle_ = db};
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(2), marker_operation::sync, 2);
    (void)co_await run(db, worker, marker_resource_type::dns_zone, zone(2), false);
    require(co_await status_of(context, "dns_zone", zone(2)) == "retrying", "failed work retries");
    co_await initiate(db, worker, marker_resource_type::dns_zone, zone(3), marker_operation::sync, 2);
    (void)co_await run(db, worker, marker_resource_type::dns_zone, zone(3), true);
    (void)co_await db.execute("UPDATE sys_dns_zone SET sync_status = 'failed' WHERE id = $1", zone(3));
    require(co_await status_of(context, "dns_zone", zone(3)) == "completed", "resource diagnostics do not rewrite task results");
    const auto attention = co_await list(context, "attention", "");
    require(listed(attention, zone(2)) && !listed(attention, zone(3)), "attention is unfinished failed work only");
    co_await initiate(db, worker, marker_resource_type::provider, dns_provider, marker_operation::verify, 1);
    const auto provider = co_await task_of(context, "provider", dns_provider);
    require(provider.get<"status">().view() == "queued" && provider.get<"resource_kind">().view() == "dns",
        "provider tasks preserve their kind");
}

// Publishes a new release of the cluster: every target node's task changes.
ruvia::task<std::string> publish(ruvia::db_handle& db, recorder& worker) {
    service::background::queued_work work;
    auto transaction = co_await db.begin_transaction();
    const auto allocated = co_await service::node_dispatch::allocate_cluster_release(transaction, tenant, cluster, 6);
    co_await service::node_dispatch::activate_cluster_release(transaction, tenant, cluster, allocated.id_,
        std::string(63, 'a') + std::to_string(allocated.generation_ % 10), "qa-only", work);
    const auto& changed = work.changed_tasks();
    require(std::find(changed.begin(), changed.end(), node_online) != changed.end() &&
                std::find(changed.begin(), changed.end(), node_offline) != changed.end(),
        "activation changes the task of every target node");
    co_await service::sync_runtime::commit_work(transaction, worker, tenant, work);
    co_return allocated.id_;
}

// The node's report of a release, as node_channel/report_service.h records it.
ruvia::task<void> report_release(ruvia::db_handle& db, std::string_view release_id, std::string_view node,
    bool applied, bool retryable = false) {
    const service::node_dispatch::node_apply_attempt attempt{.tenant_id_ = tenant,
        .release_id_ = release_id,
        .node_id_ = node,
        .source_ = "apply_result",
        .applied_ = applied,
        .phase_ = applied ? "" : "activate",
        .error_code_ = applied ? "" : "disk",
        .error_ = applied ? "" : "磁盘已满",
        .retryable_ = retryable,
        .elapsed_milliseconds_ = 400};
    auto transaction = co_await db.begin_transaction();
    const auto changed =
        applied ? co_await service::node_dispatch::mark_node_target_applied(transaction, tenant, release_id, node)
                : co_await service::node_dispatch::mark_node_target_failed(transaction, tenant, release_id, node,
                      cluster, {.phase_ = "activate", .error_code_ = "disk", .error_ = "磁盘已满", .retryable_ = retryable});
    require(changed, "the release target changed");
    const auto recorded = co_await service::node_dispatch::record_node_apply_attempt(transaction, attempt,
        std::pmr::new_delete_resource());
    require(recorded.has_value() && recorded->task_id_ == release_id, "publish attempt recorded for the release");
    co_await transaction.commit();
}

// The node's report of node spec `revision`.
ruvia::task<void> report_config(ruvia::db_handle& db, std::string_view node, std::int64_t revision, bool applied,
    bool retryable = false) {
    const service::node_dispatch::node_apply_attempt attempt{.tenant_id_ = tenant,
        .release_id_ = {},
        .node_id_ = node,
        .source_ = "apply_result",
        .applied_ = applied,
        .phase_ = applied ? "" : "validate",
        .error_code_ = applied ? "" : "endpoint_invalid",
        .error_ = applied ? "" : "节点配置校验失败",
        .retryable_ = retryable,
        .elapsed_milliseconds_ = 300};
    auto transaction = co_await db.begin_transaction();
    if (applied) {
        (void)co_await transaction.execute(
            "UPDATE sys_node SET applied_node_spec_revision = $3 WHERE tenant_id = $1 AND id = $2", tenant, node,
            revision);
    }
    const auto recorded = co_await service::node_dispatch::record_node_config_attempt(transaction, attempt, revision,
        std::pmr::new_delete_resource());
    require(recorded.has_value() && recorded->task_id_ == node && recorded->version_ == revision,
        "configure attempt recorded for the node");
    co_await transaction.commit();
}

// (d) A node task takes the worse of its release target and node spec.
ruvia::task<void> verify_node_task(ruvia::db_handle& db, read_context& context) {
    recorder worker{.handle_ = db};
    const auto first = co_await publish(db, worker);
    require(changes_task(worker.sent_.back(), node_online) && changes_task(worker.sent_.back(), node_offline),
        "the publish notifies the node tasks");
    const auto queued = co_await task_of(context, "node", node_online);
    const auto offline = co_await task_of(context, "node", node_offline);
    require(queued.get<"status">().view() == "queued" && !queued.get<"node_offline">().value_ &&
                queued.get<"name">().view() == "北京",
        "a pending target queues the node task");
    require(offline.get<"status">().view() == "queued" && offline.get<"node_offline">().value_,
        "unfinished work of an offline node is flagged");

    co_await report_release(db, first, node_online, false, false);
    const auto failed = co_await task_of(context, "node", node_online);
    require(failed.get<"status">().view() == "failed" && failed.get<"operation">().view() == "publish" &&
                failed.get<"failure_count">().value_ == 1 && failed.get<"attempt_count">().value_ == 1,
        "a non-retryable release failure fails the node task");
    const auto failures = co_await list(context, "failed", "node");
    require(listed(failures, node_online) && !listed(failures, node_offline) && failures.get<"failed">().value_ >= 1,
        "the failed filter and count");

    const auto second = co_await publish(db, worker);
    const auto requeued = co_await task_of(context, "node", node_online);
    require(requeued.get<"status">().view() == "queued" && requeued.get<"failure_count">().value_ == 0 &&
                requeued.get<"attempt_count">().value_ == 1,
        "a new release re-initiates the same node task");
    co_await report_release(db, second, node_online, true);
    const auto applied = co_await task_of(context, "node", node_online);
    require(applied.get<"status">().view() == "completed" && applied.get<"failure_count">().value_ == 0 &&
                applied.get<"attempt_count">().value_ == 2 && !applied.get<"last_success_at">().view().empty(),
        "the applied release completes the node task");
    require(co_await total_of(context, "", "node") == 2, "one task per registered node");

    co_await report_release(db, second, node_offline, true);
    const auto caught_up = co_await task_of(context, "node", node_offline);
    require(caught_up.get<"status">().view() == "completed" && !caught_up.get<"node_offline">().value_,
        "completed work is not flagged offline");

    // Node spec: a newer revision queues the node task until it is applied.
    const auto revision = integer(co_await db.query(
        "UPDATE sys_node SET node_spec_revision = node_spec_revision + 1 WHERE tenant_id = $1 AND id = $2 RETURNING "
        "node_spec_revision",
        tenant, node_online));
    require(revision == 2, "node spec revision bumped");
    require(co_await status_of(context, "node", node_online) == "queued",
        "an unapplied node spec queues the node task");
    co_await report_config(db, node_online, revision, false, true);
    const auto retrying = co_await task_of(context, "node", node_online);
    require(retrying.get<"status">().view() == "retrying" && retrying.get<"failure_count">().value_ == 1,
        "a retryable configure failure retries");
    co_await report_config(db, node_online, revision, false, false);
    const auto config_failed = co_await task_of(context, "node", node_online);
    require(config_failed.get<"status">().view() == "failed" && config_failed.get<"failure_count">().value_ == 2,
        "a non-retryable configure failure fails");
    co_await report_config(db, node_online, revision, true);
    const auto configured = co_await task_of(context, "node", node_online);
    require(configured.get<"status">().view() == "completed" && configured.get<"failure_count">().value_ == 0 &&
                configured.get<"attempt_count">().value_ == 5,
        "the applied spec completes the node task: 失败 2 次后成功");

    // (e) The node's history holds its publish and configure attempts.
    const auto history = co_await history_of(context, "node", node_online);
    const auto& attempts = history.get<"list">();
    require(attempts.size() == 5 && attempts[0].get<"operation">().view() == "configure" &&
                attempts[0].get<"outcome">().view() == "completed" &&
                attempts[2].get<"operation">().view() == "configure" &&
                attempts[2].get<"outcome">().view() == "failed" &&
                attempts[3].get<"operation">().view() == "publish" &&
                attempts[3].get<"outcome">().view() == "completed" &&
                attempts[4].get<"operation">().view() == "publish" && attempts[4].get<"error">().view() == "磁盘已满",
        "node history across releases and node specs, newest first");
    require(std::stoll(std::string(attempts[0].get<"id">().view())) >
                std::stoll(std::string(attempts[4].get<"id">().view())),
        "attempt ids descend");

    // Excluding a deleted node's targets changes its task.
    service::background::queued_work work;
    auto transaction = co_await db.begin_transaction();
    (void)co_await transaction.execute(
        "UPDATE sys_node_release_target SET status = 'pending' WHERE tenant_id = $1 AND release_id = $2 AND node_id = "
        "$3",
        tenant, second, node_offline);
    co_await service::node_dispatch::exclude_pending_node_targets(transaction, std::string(tenant),
        std::string(node_offline), work);
    require(work.changed_tasks() == std::vector<std::string>{std::string(node_offline)},
        "excluding a node's targets changes its task");
    co_await service::sync_runtime::commit_work(transaction, worker, tenant, work);
    require(changes_task(worker.sent_.back(), node_offline), "the exclusion notifies the node task");
}

ruvia::task<void> verify_node_snapshot(ruvia::db_handle& db, read_context& context) {
    recorder worker{.handle_ = db};
    const auto revision = integer(co_await db.query(
        "UPDATE sys_node SET node_spec_revision = node_spec_revision + 1 WHERE id = $1 RETURNING node_spec_revision",
        node_online));
    co_await report_config(db, node_online, revision, false, false);
    const auto before = co_await task_of(context, "node", node_online);
    (void)co_await db.execute(
        "UPDATE sys_sync_event SET emitted_at = NOW() - INTERVAL '8 days' WHERE resource_id = $1", node_online);
    (void)co_await db.execute("UPDATE sys_scheduled_job SET next_run_at = NOW() - INTERVAL '1 hour' WHERE name = $1",
        service::sync_runtime::retention_job);
    co_await service::sync_runtime::prune_result_events(worker);
    const auto after = co_await task_of(context, "node", node_online);
    require(after.get<"status">().view() == "failed" && after.get<"failure_count">().value_ == 1 &&
                after.get<"error">().view() == before.get<"error">().view() &&
                after.get<"updated_at">().view() == before.get<"updated_at">().view() &&
                after.get<"attempt_count">().value_ == 0,
        "history retention never changes the current node task result or timestamp");

    (void)co_await db.execute("UPDATE sys_node SET node_spec_revision = node_spec_revision + 1 WHERE id = $1", node_online);
    const auto queued = co_await task_of(context, "node", node_online);
    require(queued.get<"status">().view() == "queued" && queued.get<"failure_count">().value_ == 0 && queued.get<"error">().empty(),
        "a new node spec does not reuse a previous revision's failure");
    co_await report_config(db, node_online, revision + 1, false, true);
    const auto release = co_await publish(db, worker);
    co_await report_release(db, release, node_online, false, false);
    const auto publish_failed = co_await task_of(context, "node", node_online);
    require(publish_failed.get<"operation">().view() == "publish" && publish_failed.get<"status">().view() == "failed" &&
                publish_failed.get<"error">().view() == "磁盘已满" && publish_failed.get<"failure_count">().value_ == 1,
        "operation, status, error and failure count describe the same selected work");
    co_await report_config(db, node_online, revision + 1, false, false);
    const auto configure_failed = co_await task_of(context, "node", node_online);
    require(configure_failed.get<"operation">().view() == "configure" &&
                configure_failed.get<"error">().view() == "节点配置校验失败" && configure_failed.get<"failure_count">().value_ == 2,
        "equal severity selects the pending spec with its own error and counter");
    co_await report_release(db, release, node_online, true);
    const auto still_failed = co_await task_of(context, "node", node_online);
    require(still_failed.get<"failure_count">().value_ == 2,
        "a successful release does not erase the separate configure failure");
    co_await report_config(db, node_online, revision + 1, true);
    const auto finished = co_await task_of(context, "node", node_online);
    require(finished.get<"failure_count">().value_ == 0,
        "the current configure success clears its failures");
}

ruvia::task<void> verify_tasks(ruvia::db_client& client) {
    co_await client.connect();
    auto db = client.handle();
    std::pmr::monotonic_buffer_resource pool;
    read_context context{.handle_ = client.handle(), .pool_ = &pool};
    co_await insert_fixture(db);
    co_await verify_marker_task(db, context);
    co_await verify_result_status(db, context);
    co_await verify_node_task(db, context);
    co_await verify_terminal_failure(db, context);
    co_await verify_query_pagination(context);
    co_await verify_node_snapshot(db, context);
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
    const auto& all = service::config::schema_migrations;
    try {
        const test_support::postgres_test_database fresh(*base);
        if (migrate(fresh, all, all.size(), "fresh install") != 0 ||
            fresh.run(verify_tasks) != 0) {
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Task center migration, marker and node tasks, warnings and history passed\n";
    return 0;
}
