#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/web/ModelJson.h>

#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/dns_sync/task.h"
#include "service/features/logging/logger.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"

namespace service::dns_sync::detail {

inline ruvia::Task<void>
importInitialRemoteRecords(service::background::WorkerContext& context, const DnsTask& task,
                           const service::sync_runtime::RunningMarkerLease& lease,
                           std::int64_t configRevision, std::string_view domain,
                           const ZoneRuntimeData& currentRuntime,
                           const service::dns::DnsProviderDriver& driver,
                           const std::vector<service::dns::ProviderRecord>& remoteRecords,
                           const std::vector<service::dns::ProviderLine>& lines) {
    if (remoteRecords.size() > kMaxReconciliationRecords) {
        throw std::runtime_error("远程 DNS 记录超过单个域名可导入上限");
    }

    std::unordered_set<std::string> enabledLines;
    enabledLines.reserve(lines.size());
    for (const auto& line : lines) {
        if (!line.code.empty()) {
            enabledLines.emplace(line.code);
        }
    }

    std::unordered_set<std::string> remoteIds;
    remoteIds.reserve(remoteRecords.size());
    for (const auto& record : remoteRecords) {
        if (!supportsManagedRecordType(record.type)) {
            throw std::runtime_error("远程 DNS 存在当前不支持自动托管的记录类型：" + record.type);
        }
        if (record.id.empty() || record.name.empty() || record.content.empty() || record.ttl < 1 ||
            record.ttl > 86400 ||
            (record.priority && (*record.priority < 0 || *record.priority > 65535)) ||
            !enabledLines.contains(record.lineCode)) {
            throw std::runtime_error("远程 DNS 记录不符合本地托管配置要求");
        }
        if (!remoteIds.emplace(record.id).second) {
            throw std::runtime_error("远程 DNS 记录 ID 重复");
        }
    }

    auto transaction = co_await context.db().beginTransaction();
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)",
        static_cast<std::int64_t>(remoteRecords.size()));
    if (ids.size() != remoteRecords.size()) {
        throw std::runtime_error("远程 DNS 记录本地 ID 生成失败");
    }

    ZoneConfigOutput importedConfig({.resource = context.resource()});
    auto& configRecords = importedConfig.ensure<"records">();
    ZoneRuntimeDto importedRuntime({.resource = context.resource()});
    importedRuntime.set<"recordsImported">(true);
    auto& runtimeLines = importedRuntime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& outputLine =
            runtimeLines.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        outputLine.set<"code">(line.code);
        outputLine.set<"name">(line.name);
        outputLine.set<"displayName">(line.displayName);
        outputLine.set<"status">("enabled");
    }
    auto& states = importedRuntime.ensure<"recordStates">();
    (void)importedRuntime.ensure<"conflicts">();
    appendChallengeRecords(importedRuntime, currentRuntime);
    for (std::size_t index = 0; index < remoteRecords.size(); ++index) {
        const auto& remote = remoteRecords[index];
        const auto id = std::string(ids[index][0].value().value_or(""));
        if (id.empty()) {
            throw std::runtime_error("远程 DNS 记录本地 ID 为空");
        }
        auto& record =
            configRecords.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        record.set<"id">(id);
        record.set<"type">(remote.type);
        record.set<"name">(driver.localRecordName(remote.name, domain));
        record.set<"content">(remote.content);
        record.set<"ttl">(remote.ttl);
        record.set<"proxied">(remote.proxied);
        record.set<"lineCode">(remote.lineCode);
        if (remote.priority) {
            record.set<"priority">(*remote.priority);
        }
        auto& state = states.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        state.set<"id">(id);
        state.set<"remoteRecordId">(remote.id);
        state.set<"syncStatus">("pending");
        state.set<"syncedRevision">(0);
    }

    const auto configJson = ruvia::toJson(importedConfig, {.resource = context.resource()});
    const auto runtimeJson = ruvia::toJson(importedRuntime, {.resource = context.resource()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, runtime = $2::jsonb, revision = revision + "
        "1, desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
        "desired_revision = $6 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(configJson), std::string_view(runtimeJson), task.resourceId, task.tenantId,
        configRevision, task.version);
    if (updated.empty()) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return;
    }
    if (!co_await service::sync_runtime::releaseRunning(transaction, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    (void)co_await enqueueZoneRevision(
        transaction, task.tenantId, task.resourceId,
        updated.front()[0].as<std::int64_t>().value_or(task.version + 1));
    co_await transaction.commit();
    co_return;
}

inline ruvia::Task<void>
storeConflicts(service::background::WorkerContext& context, const DnsTask& task,
               const service::sync_runtime::RunningMarkerLease& lease, std::int64_t desiredRevision,
               const ZoneRuntimeData& runtime, const std::vector<service::dns::ProviderLine>& lines,
               const std::vector<RecordConflict>& conflicts) {
    ZoneRuntimeDto nextRuntime({.resource = context.resource()});
    nextRuntime.set<"recordsImported">(true);
    auto& runtimeLines = nextRuntime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& outputLine =
            runtimeLines.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        outputLine.set<"code">(line.code);
        outputLine.set<"name">(line.name);
        outputLine.set<"displayName">(line.displayName);
        outputLine.set<"status">("enabled");
    }
    auto& states = nextRuntime.ensure<"recordStates">();
    (void)nextRuntime.ensure<"conflicts">();
    for (const auto& state : runtime.recordStates) {
        if (!state.id) {
            continue;
        }
        auto& output = states.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        output.set<"id">(*state.id);
        output.set<"syncStatus">(state.syncStatus ? *state.syncStatus
                                                  : std::string_view{"pending"});
        output.set<"syncedRevision">(state.syncedRevision ? *state.syncedRevision : 0);
        if (state.remoteRecordId) {
            output.set<"remoteRecordId">(*state.remoteRecordId);
        }
        if (state.lastError) {
            output.set<"lastError">(*state.lastError);
        }
    }
    auto& outputConflicts = nextRuntime.ensure<"conflicts">();
    for (const auto& conflict : conflicts) {
        auto& outputConflict =
            outputConflicts.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        outputConflict.set<"id">(conflict.id);
        outputConflict.set<"type">(conflict.type);
        outputConflict.set<"name">(conflict.name);
        outputConflict.set<"lineCode">(conflict.lineCode);
        outputConflict.set<"localContent">(conflict.localContent);
        outputConflict.set<"remoteContent">(conflict.remoteContent);
    }
    appendChallengeRecords(nextRuntime, runtime);

    const auto runtimeJson = ruvia::toJson(nextRuntime, {.resource = context.resource()});
    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET runtime = $1::jsonb, sync_status = 'conflict', last_error = "
        "NULL, updated_at = NOW() WHERE id = $2 AND tenant_id = $3 AND desired_revision = "
        "$4 AND deleted_at IS NULL",
        std::string_view(runtimeJson), task.resourceId, task.tenantId, desiredRevision);
    service::sync_runtime::RunningResultTransition resultTransition;
    if (updated.affectedRows() != 0) {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        resultTransition =
            co_await service::sync_runtime::completeRunningAndRecordEvent(transaction, lease);
        if (!resultTransition.markerTransitioned) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    co_return;
}

inline ruvia::Task<bool> persistRemoteMerge(service::background::WorkerContext& context,
                                            const DnsTask& task,
                                            const service::sync_runtime::RunningMarkerLease& lease,
                                            std::int64_t configRevision,
                                            std::int64_t desiredRevision, RemoteMergePlan plan,
                                            const ZoneRuntimeData& runtime,
                                            const std::vector<service::dns::ProviderLine>& lines) {
    auto& merged = plan.records;
    auto& remoteIdsByLocalId = plan.remoteIdsByLocalId;
    auto transaction = co_await context.db().beginTransaction();
    const auto newIdCount = static_cast<std::int64_t>(std::count_if(
        merged.begin(), merged.end(), [](const auto& record) { return record.id.empty(); }));
    const auto ids = co_await transaction.query(
        "SELECT gen_random_uuid()::TEXT FROM generate_series(1, $1::BIGINT)", newIdCount);
    if (ids.size() != static_cast<std::size_t>(newIdCount)) {
        throw std::runtime_error("远程 DNS 记录本地 ID 生成失败");
    }
    std::size_t nextId = 0;
    for (auto& record : merged) {
        if (!record.id.empty()) {
            continue;
        }
        record.id = std::string(ids[nextId++][0].value().value_or(""));
        if (record.id.empty()) {
            throw std::runtime_error("远程 DNS 记录本地 ID 为空");
        }
        if (record.remoteId.empty()) {
            throw std::runtime_error("远程 DNS 记录合并状态损坏");
        }
        remoteIdsByLocalId.emplace(record.id, record.remoteId);
    }

    ZoneConfigOutput mergedConfig({.resource = context.resource()});
    auto& configRecords = mergedConfig.ensure<"records">();
    for (const auto& record : merged) {
        auto& output =
            configRecords.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        output.set<"id">(record.id);
        output.set<"type">(record.type);
        output.set<"name">(record.name);
        output.set<"content">(record.content);
        output.set<"ttl">(record.ttl);
        output.set<"proxied">(record.proxied);
        output.set<"lineCode">(record.lineCode);
        if (record.priority) {
            output.set<"priority">(*record.priority);
        }
    }

    ZoneRuntimeDto mergedRuntime({.resource = context.resource()});
    mergedRuntime.set<"recordsImported">(true);
    auto& runtimeLines = mergedRuntime.ensure<"lines">();
    for (const auto& line : lines) {
        auto& outputLine =
            runtimeLines.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        outputLine.set<"code">(line.code);
        outputLine.set<"name">(line.name);
        outputLine.set<"displayName">(line.displayName);
        outputLine.set<"status">("enabled");
    }
    auto& states = mergedRuntime.ensure<"recordStates">();
    (void)mergedRuntime.ensure<"conflicts">();
    for (const auto& [id, remoteId] : remoteIdsByLocalId) {
        auto& state = states.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
        state.set<"id">(id);
        state.set<"remoteRecordId">(remoteId);
        state.set<"syncStatus">("pending");
        state.set<"syncedRevision">(0);
    }
    appendChallengeRecords(mergedRuntime, runtime);

    const auto configJson = ruvia::toJson(mergedConfig, {.resource = context.resource()});
    const auto runtimeJson = ruvia::toJson(mergedRuntime, {.resource = context.resource()});
    const auto updated = co_await transaction.query(
        "UPDATE sys_dns_zone SET config = $1::jsonb, runtime = $2::jsonb, revision = revision + "
        "1, desired_revision = desired_revision + 1, sync_status = 'pending', last_error = NULL, "
        "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
        "desired_revision = $6 AND deleted_at IS NULL RETURNING desired_revision",
        std::string_view(configJson), std::string_view(runtimeJson), task.resourceId, task.tenantId,
        configRevision, desiredRevision);
    if (updated.empty()) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return false;
    }
    if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    (void)co_await enqueueZoneRevision(
        transaction, task.tenantId, task.resourceId,
        updated.front()[0].as<std::int64_t>().value_or(desiredRevision + 1), task.operation);
    (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    co_await transaction.commit();
    co_return true;
}

inline ruvia::Task<void> persistSyncedZone(service::background::WorkerContext& context,
                                           const DnsTask& task,
                                           const service::sync_runtime::RunningMarkerLease& lease,
                                           const ZoneRuntimeDto& runtime, std::int64_t revision) {
    const auto runtimeJson = ruvia::toJson(runtime, {.resource = context.resource()});
    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET runtime = $2::jsonb, synced_revision = GREATEST("
        "synced_revision, $3), sync_status = CASE WHEN desired_revision <= $3 THEN 'synced' ELSE "
        "'pending' END, last_synced_at = NOW(), last_error = NULL, updated_at = NOW() WHERE id = "
        "$1 AND tenant_id = $4 AND desired_revision = $3",
        task.resourceId, std::string_view(runtimeJson), revision, task.tenantId);
    service::sync_runtime::RunningResultTransition resultTransition;
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    } else {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        if (task.operation == service::sync_runtime::MarkerOperation::remove) {
            if (!co_await service::sync_runtime::removeRunning(transaction, lease)) {
                throw std::runtime_error("DNS 删除同步标记 lease 已失效");
            }
        } else {
            resultTransition =
                co_await service::sync_runtime::completeRunningAndRecordEvent(transaction, lease);
            if (!resultTransition.markerTransitioned) {
                throw std::runtime_error("DNS 同步标记 lease 已失效");
            }
        }
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    co_return;
}

inline ruvia::Task<void>
finishDeletedZoneWithoutRemote(service::background::WorkerContext& context, const DnsTask& task,
                               const service::sync_runtime::RunningMarkerLease& lease) {
    auto transaction = co_await context.db().beginTransaction();
    const auto updated = co_await transaction.execute(
        "UPDATE sys_dns_zone SET synced_revision = GREATEST(synced_revision, $3), "
        "sync_status = 'synced', last_synced_at = NOW(), last_error = NULL, updated_at = NOW() "
        "WHERE id = $1 AND tenant_id = $2 AND desired_revision = $3",
        task.resourceId, task.tenantId, task.version);
    if (updated.affectedRows() != 0 &&
        !co_await service::sync_runtime::removeRunning(transaction, lease)) {
        throw std::runtime_error("DNS 删除同步标记 lease 已失效");
    }
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    }
    co_await transaction.commit();
    co_return;
}

inline ruvia::Task<void> failDnsTask(service::background::WorkerContext& context,
                                     const DnsTask& task, std::string_view error, bool permanent) {
    const auto message = service::sync_runtime::boundedError(error);
    const auto lease = service::sync_runtime::makeRunningLease(task.tenantId, task.id, task.version,
                                                               context.leaseOwner());
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenantId, task.resourceId);
    const auto resultTransition =
        co_await service::sync_runtime::failRunningAndRecordEvent(transaction, lease, message);
    if (resultTransition.markerTransitioned) {
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET sync_status = $2, last_error = $3, updated_at = NOW() WHERE "
            "id = $1 AND desired_revision = $4 AND tenant_id = $5",
            task.resourceId, permanent ? std::string_view{"failed"} : std::string_view{"pending"},
            std::string_view(message), task.version, task.tenantId);
    }
    co_await service::sync_runtime::commitAndPublishResultEvent(transaction, lease,
                                                                resultTransition);
    if (resultTransition.markerTransitioned) {
        service::logging::error("DNS sync task " + task.id + " failed: " + message);
    }
    co_return;
}

} // namespace service::dns_sync::detail
