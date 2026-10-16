#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/features/background/task_context.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/mapper.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/dns_sync/task.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/sync_runtime/work_commit.h"

namespace service::dns_sync::detail {

// A routine check can finish without changing a task or emitting a duplicate
// resource event. Actual changes and explicit syncs retain their execution result.
template <typename context_type>
inline ruvia::task<void> persist_synced_zone(context_type& context,
    service::attempt_trace::recorder& trace, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease,
    const zone_snapshot& snapshot, std::int64_t revision, std::int64_t snapshot_revision, std::string_view unsynced_error,
    std::string_view detail, bool provider_changed) {
    const auto config_json = ruvia::to_json(to_output(snapshot.records_), {.resource_ = context.pool()});
    const auto runtime_json = ruvia::to_json(to_output(snapshot.runtime_), {.resource_ = context.pool()});
    const std::string_view status = unsynced_error.empty() ? "synced" : "failed";
    auto transaction = co_await context.db().begin_transaction();
    const auto current = co_await transaction.query(
        "SELECT config IS DISTINCT FROM $3::jsonb, runtime IS DISTINCT FROM $4::jsonb OR "
        "sync_status <> $5 OR synced_revision <> $6 OR COALESCE(last_error, '') <> $7 "
        "FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND desired_revision = $6 AND revision = $8 FOR UPDATE",
        task.tenant_id_, task.resource_id_, std::string_view(config_json), std::string_view(runtime_json),
        status, revision, unsynced_error, snapshot_revision);
    if (current.empty()) {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::renew_running_lease(transaction, lease)) {
        throw std::runtime_error("DNS 同步任务 lease 已失效");
    }
    const bool records_changed = current.front()[0].template as<bool>().value_or(true);
    const bool state_changed = current.front()[1].template as<bool>().value_or(true);
    if (task.inspection_ && !records_changed && !state_changed && !provider_changed) {
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET next_check_at = NOW() + INTERVAL '10 minutes' "
            "+ random() * INTERVAL '1 minute' WHERE tenant_id = $1 AND id = $2",
            task.tenant_id_, task.resource_id_);
        (void)co_await transaction.execute(
            "UPDATE sys_sync_task SET lease_owner = NULL, lease_until = NULL "
            "WHERE tenant_id = $1 AND id = $2 AND version = $3 AND lease_owner = $4",
            task.tenant_id_, task.id_, task.version_, context.lease_owner());
        co_await transaction.commit();
        co_return;
    }
    (void)co_await transaction.execute(
        "UPDATE sys_dns_zone SET config = $3::jsonb, runtime = $4::jsonb, "
        "revision = revision + $8, synced_revision = $6, sync_status = $5, "
        "last_synced_at = NOW(), last_error = NULLIF($7::text, ''), updated_at = NOW(), "
        "next_check_at = NOW() + INTERVAL '10 minutes' + random() * INTERVAL '1 minute' "
        "WHERE tenant_id = $1 AND id = $2 AND desired_revision = $6",
        task.tenant_id_, task.resource_id_, std::string_view(config_json), std::string_view(runtime_json),
        status, revision, unsynced_error, std::int64_t{records_changed ? 1 : 0});
    service::sync_runtime::running_result_transition transition;
    if (task.operation_ == service::sync_runtime::marker_operation::remove) {
        if (!co_await service::sync_runtime::remove_running(transaction, lease)) {
            throw std::runtime_error("DNS 删除任务 lease 已失效");
        }
        transition.marker_transitioned_ = true;
        transition.resumed_ = co_await service::sync_runtime::resume_waiters(transaction, lease, true);
    } else {
        transition = co_await service::sync_runtime::record_running_result(transaction, lease,
            unsynced_error.empty() ? service::sync_runtime::attempt_result::completed()
                                   : service::sync_runtime::attempt_result::failed(unsynced_error),
            detail);
        if (!transition.marker_transitioned_) {
            throw std::runtime_error("DNS 同步任务 lease 已失效");
        }
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease, transition, trace);
}

inline ruvia::task<void>
finish_deleted_zone_without_remote(service::background::task_context& context, const dns_task& task,
    const service::sync_runtime::running_marker_lease& lease) {
    auto transaction = co_await context.db().begin_transaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET synced_revision = GREATEST(synced_revision, $3), "
        "sync_status = 'synced', last_synced_at = NOW(), last_error = NULL, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND desired_revision = $3",
        task.resource_id_, task.tenant_id_, task.version_);
    bool removed = false;
    std::vector<service::sync_runtime::marker_waiter> resumed;
    if (updated.affected_rows() != 0) {
        removed = co_await service::sync_runtime::remove_running(transaction, lease);
        if (!removed) {
            throw std::runtime_error("DNS 删除同步标记 lease 已失效");
        }
        resumed = co_await service::sync_runtime::resume_waiters(transaction, lease, true);
    } else {
        (void)co_await service::sync_runtime::release_running(transaction, lease);
    }
    if (removed) {
        service::live_resource::notifications::resource_notification notification(task.tenant_id_);
        notification.changed(service::live_resource::resource::tasks, task.resource_id_);
        for (const auto& waiter : resumed) {
            notification.changed(service::live_resource::resource::tasks, waiter.resource_id_);
        }
        (void)co_await service::live_resource::enqueue_notification(transaction, notification);
    }
    co_await transaction.commit();
    co_return;
}

inline ruvia::task<void> fail_dns_task(service::background::task_context& context,
    service::attempt_trace::recorder& trace, const dns_task& task, std::string_view error, bool permanent,
    std::string_view detail) {
    const auto message = service::sync_runtime::attempt_error(trace, error);
    const auto lease = service::sync_runtime::make_running_lease(
        task.tenant_id_, task.id_, task.version_, context.lease_owner(), task.claimed_at_,
        service::sync_runtime::marker_resource_type::dns_zone, task.resource_id_);
    auto transaction = co_await context.db().begin_transaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenant_id_, task.resource_id_);
    if (task.inspection_) {
        (void)co_await transaction.execute(
            "UPDATE sys_sync_task SET count_fails = 0 WHERE tenant_id = $1 AND id = $2 "
            "AND version = $3 AND lease_owner = $4",
            task.tenant_id_, task.id_, task.version_, context.lease_owner());
    }
    const auto result_transition = co_await service::sync_runtime::record_running_result(
        transaction, lease, permanent ? service::sync_runtime::attempt_result::failed(message) : service::sync_runtime::attempt_result::retry(message), detail);
    if (result_transition.marker_transitioned_) {
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET sync_status = $2, last_error = $3, updated_at = NOW(), "
            "next_check_at = NOW() + INTERVAL '10 minutes' WHERE "
            "id = $1 AND desired_revision = $4 AND tenant_id = $5",
            task.resource_id_, permanent ? std::string_view{"failed"} : std::string_view{"pending"},
            std::string_view(message), task.version_, task.tenant_id_);
    }
    co_await service::sync_runtime::commit_and_publish_result_event(transaction, context, lease,
        result_transition, trace);
    if (result_transition.marker_transitioned_) {
        service::sync_runtime::log_attempt_failure("DNS sync task", task.id_, message);
    }
    co_return;
}

}  // namespace service::dns_sync::detail
