#pragma once
#include <ruvia/core/timer.h>
#include <ruvia/web/model_json.h>

#include <chrono>

#include "service/features/background/task_context.h"
#include "service/features/background/work_available.h"
#include "service/features/cache_tasks/transport.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/attempt_record.h"
#include "service/utils/token.h"
namespace service::cache_tasks {
inline ruvia::task<void> record_result(ruvia::db_transaction& tx, const std::string& tenant,
    const std::string& node, const flexedge::node::v2::CacheResult& result) {
    ruvia::array<ruvia::string> failures;
    for (const auto& failure : result.failures()) {
        failures.emplace_back(failure);
    }
    const auto rows = co_await tx.query(
        "UPDATE sys_cache_task_target target SET status=$6,error=$7,affected_objects=$8,affected_bytes=$9,completed_urls=$10,failures=$11::jsonb,updated_at=NOW() "
        "FROM sys_cache_task task WHERE target.tenant_id=$1 AND target.node_id=$2 AND target.task_id=$3 "
        "AND target.generation=$4 AND target.attempt_id=$5 AND target.status='sent' AND task.tenant_id=target.tenant_id "
        "AND task.id=target.task_id AND task.generation=target.generation RETURNING target.task_id",
        tenant, node, result.task_id(), result.generation(), result.attempt_id(), result.ok() ? "completed" : "failed",
        result.error(), static_cast<std::int64_t>(result.affected_objects()), static_cast<std::int64_t>(result.affected_bytes()),
        static_cast<std::int64_t>(result.completed_urls()), ruvia::to_json(failures));
    if (!rows.empty()) {
        service::live_resource::notifications::resource_notification notice(tenant);
        notice.changed(service::live_resource::resource::tasks, result.task_id());
        (void)co_await service::live_resource::enqueue_notification(tx, notice);
    }
}

inline ruvia::task<bool> run_one(service::background::task_context& c) {
    auto tx = co_await c.db().begin_transaction();
    const auto claimed = co_await tx.query(
        "WITH candidate AS (SELECT id FROM sys_cache_task WHERE status IN ('queued','running','retrying') "
        "AND next_attempt_at<=NOW() AND (lease_until IS NULL OR lease_until<NOW()) ORDER BY next_attempt_at,id FOR UPDATE SKIP LOCKED LIMIT 1) "
        "UPDATE sys_cache_task task SET lease_owner=$1,lease_until=NOW()+INTERVAL '45 seconds',status='running',started_at=NOW(),"
        "attempt_count=attempt_count+1,updated_at=NOW() FROM candidate WHERE task.id=candidate.id RETURNING task.id,task.tenant_id,task.generation",
        c.lease_owner());
    if (claimed.empty()) {
        co_await tx.commit();
        co_return false;
    }
    const auto id = std::string(claimed.front()[0].value().value());
    const auto tenant = std::string(claimed.front()[1].value().value());
    const auto generation = claimed.front()[2].as<std::int64_t>().value();
    service::live_resource::notifications::resource_notification changed(tenant);
    changed.changed(service::live_resource::resource::tasks, id);
    co_await service::live_resource::commit_and_notify(tx, c, changed);
    const auto targets = co_await c.db().query(
        "SELECT target.node_id,target.command_envelope,node.response_cache_protocol FROM sys_cache_task_target target "
        "JOIN sys_node node ON node.tenant_id=target.tenant_id AND node.id=target.node_id "
        "WHERE target.tenant_id=$1 AND target.task_id=$2 AND target.generation=$3 AND target.status<>'completed'",
        tenant, id, generation);
    std::vector<std::pair<std::string, flexedge::node::v2::CacheCommand>> commands;
    for (const auto& target : targets) {
        const auto node = std::string(target[0].value().value());
        auto command = decode(target[1].value().value());
        if (!command) {
            throw std::runtime_error("invalid durable cache command");
        }
        if (target[2].as<std::int64_t>().value() < 1) {
            (void)co_await c.db().execute("UPDATE sys_cache_task_target SET status='failed',error='节点版本尚不支持缓存操作，请先升级节点',updated_at=NOW() WHERE tenant_id=$1 AND task_id=$2 AND generation=$3 AND node_id=$4", tenant, id, generation, node);
            continue;
        }
        command->set_attempt_id(service::utils::random_token());
        command->set_expires_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 600000);
        const auto staged = co_await c.db().query(
            "UPDATE sys_cache_task_target target SET status='sent',error='',attempt_id=$5,updated_at=NOW() FROM sys_cache_task task "
            "WHERE target.tenant_id=$1 AND target.task_id=$2 AND target.generation=$3 AND target.node_id=$4 AND task.id=target.task_id "
            "AND task.generation=target.generation AND task.lease_owner=$6 RETURNING target.node_id",
            tenant, id, generation, node, command->attempt_id(), c.lease_owner());
        if (!staged.empty()) {
            co_await enqueue(c.redis(), node, *command);
            commands.emplace_back(node, std::move(*command));
        }
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    auto renew_at = std::chrono::steady_clock::now();
    for (;;) {
        if (c.get_stop_token().stop_requested()) {
            co_return true;
        }
        const auto pending = co_await c.db().query("SELECT node_id,attempt_id FROM sys_cache_task_target WHERE tenant_id=$1 AND task_id=$2 AND generation=$3 AND status IN ('pending','sent')", tenant, id, generation);
        if (pending.empty() || std::chrono::steady_clock::now() >= deadline) {
            break;
        }
        if (std::chrono::steady_clock::now() >= renew_at) {
            const auto renewed = co_await c.db().query("UPDATE sys_cache_task SET lease_until=NOW()+INTERVAL '45 seconds' WHERE tenant_id=$1 AND id=$2 AND generation=$3 AND lease_owner=$4 RETURNING id", tenant, id, generation, c.lease_owner());
            if (renewed.empty()) {
                co_return true;
            }
            for (const auto& [node, command] : commands) {
                const bool waiting = std::ranges::any_of(pending, [&](const auto& target) { return target[0].value().value() == node && target[1].value().value() == command.attempt_id(); });
                if (waiting) {
                    co_await enqueue(c.redis(), node, command);
                }
            }
            renew_at = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
        (void)co_await ruvia::sleep_for(c.worker(), std::chrono::seconds(1), c.get_stop_token());
    }
    auto finish = co_await c.db().begin_transaction();
    const auto owned = co_await finish.query("SELECT attempt_count,operation,started_at FROM sys_cache_task WHERE tenant_id=$1 AND id=$2 AND generation=$3 AND lease_owner=$4 FOR UPDATE", tenant, id, generation, c.lease_owner());
    if (owned.empty()) {
        co_await finish.commit();
        co_return true;
    }
    const auto unfinished = co_await finish.query("SELECT COUNT(*) FROM sys_cache_task_target WHERE tenant_id=$1 AND task_id=$2 AND generation=$3 AND status<>'completed'", tenant, id, generation);
    const bool ok = unfinished.front()[0].as<std::int64_t>().value() == 0;
    const std::string error = ok ? "" : "部分节点未完成，请查看节点执行结果；失败任务将自动重试";
    const auto updated = co_await finish.query(
        "UPDATE sys_cache_task SET status=CASE WHEN $5 THEN 'completed' WHEN failure_count>=2 THEN 'failed' ELSE 'retrying' END,"
        "failure_count=CASE WHEN $5 THEN 0 ELSE failure_count+1 END,error=$6,lease_owner=NULL,lease_until=NULL,"
        "next_attempt_at=NOW()+INTERVAL '10 seconds',last_success_at=CASE WHEN $5 THEN NOW() ELSE last_success_at END,updated_at=NOW() "
        "WHERE tenant_id=$1 AND id=$2 AND generation=$3 AND lease_owner=$4 RETURNING id",
        tenant, id, generation, c.lease_owner(), ok, error);
    if (!updated.empty()) {
        const auto events = co_await finish.query(
            "INSERT INTO sys_sync_event(tenant_id,task_id,resource_type,resource_id,operation,version,outcome,error,attempt,started_at,duration_ms) "
            "SELECT tenant_id,id,'cache',id,operation,generation,$3,$4,attempt_count::integer,started_at,"
            "GREATEST(0,(EXTRACT(EPOCH FROM NOW()-started_at)*1000)::bigint) FROM sys_cache_task WHERE tenant_id=$1 AND id=$2 RETURNING id",
            tenant, id, ok ? "completed" : "failed", error);
        service::live_resource::notifications::resource_notification notice(tenant);
        notice.changed(service::live_resource::resource::tasks, id);
        if (!events.empty()) {
            notice.task_attempt(id, events.front()[0].as<std::int64_t>().value());
        }
        co_await service::live_resource::commit_and_notify(finish, c, notice);
    } else {
        co_await finish.commit();
    }
    co_return true;
}

inline ruvia::task<void> run_worker(service::background::task_context& c) {
    auto wake = service::background::work_available::subscribe(c, "cache-tasks");
    while (!c.get_stop_token().stop_requested()) {
        bool claimed{};
        try {
            claimed = co_await run_one(c);
        } catch (const std::exception& error) {
            if (!c.get_stop_token().stop_requested()) {
                service::logging::error(std::string("Cache task: ") + error.what());
            }
        }
        if (!claimed) {
            (void)co_await wake.receive_for(std::chrono::seconds(5), c.get_stop_token());
        }
    }
}
}  // namespace service::cache_tasks
