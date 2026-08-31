#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/ModelJson.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns/provider_config.h"
#include "service/features/dns_sync/queue.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/sync_runtime/state.h"
#include "service/features/logging/logger.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::dns_sync {

namespace detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{32};
inline constexpr std::size_t kMaxInitialImportedRecords{10'000};

struct DnsTask final {
    std::string id;
    std::string tenantId;
    std::string resourceId;
    std::string operation;
    std::int64_t version;
    std::int64_t failures;
};

inline std::string boundedError(std::string_view value) {
    constexpr std::size_t limit{1000};
    return std::string(value.substr(0, std::min(value.size(), limit)));
}

inline bool supportsManagedRecordType(std::string_view type) {
    return type == "A" || type == "AAAA" || type == "CNAME" || type == "TXT" || type == "MX";
}

inline ruvia::Task<void>
importInitialRemoteRecords(service::background::WorkerContext& context, const DnsTask& task,
                           const service::sync_runtime::RunningMarkerLease& lease,
                           std::int64_t configRevision, std::string_view domain,
                           const ZoneRuntimeData& currentRuntime,
                           const service::dns::DnsProviderDriver& driver,
                           const std::vector<service::dns::ProviderRecord>& remoteRecords,
                           const std::vector<service::dns::ProviderLine>& lines) {
    if (remoteRecords.size() > kMaxInitialImportedRecords) {
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

struct ManagedRecord final {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string lineCode;
    std::string remoteId{};
};

inline std::string recordIdentity(const service::dns::ProviderRecord& record) {
    return record.type + "\x1f" + service::common::normalizeDomainName(record.name) + "\x1f" +
           record.lineCode;
}

inline service::dns::ProviderRecord toRemoteRecord(const ManagedRecord& record,
                                                   const service::dns::DnsProviderDriver& driver,
                                                   std::string_view domain) {
    return {
        .id = {},
        .type = record.type,
        .name = driver.remoteRecordName(record.name, domain),
        .content = record.content,
        .ttl = record.ttl,
        .priority = record.priority,
        .proxied = record.proxied,
        .lineCode = record.lineCode,
    };
}

inline bool recordMatches(const ManagedRecord& local, const service::dns::ProviderRecord& remote,
                          const service::dns::DnsProviderDriver& driver, std::string_view domain) {
    const auto expected = toRemoteRecord(local, driver, domain);
    return expected.type == remote.type && expected.content == remote.content &&
           expected.ttl == remote.ttl && expected.priority == remote.priority &&
           expected.proxied == remote.proxied && expected.lineCode == remote.lineCode &&
           service::common::normalizeDomainName(expected.name) ==
               service::common::normalizeDomainName(remote.name);
}

inline void applyRemoteRecord(ManagedRecord& target, const service::dns::ProviderRecord& source,
                              const service::dns::DnsProviderDriver& driver,
                              std::string_view domain) {
    target.type = source.type;
    target.name = driver.localRecordName(source.name, domain);
    target.content = source.content;
    target.ttl = source.ttl;
    target.priority = source.priority;
    target.proxied = source.proxied;
    target.lineCode = source.lineCode;
}

inline void validateRemoteRecord(const service::dns::ProviderRecord& record,
                                 const std::unordered_set<std::string>& enabledLines) {
    if (!supportsManagedRecordType(record.type)) {
        throw std::runtime_error("远程 DNS 存在当前不支持自动托管的记录类型：" + record.type);
    }
    if (record.id.empty() || record.name.empty() || record.content.empty() || record.ttl < 1 ||
        record.ttl > 86400 ||
        (record.priority && (*record.priority < 0 || *record.priority > 65535)) ||
        !enabledLines.contains(record.lineCode)) {
        throw std::runtime_error("远程 DNS 记录不符合本地托管配置要求");
    }
}

struct RecordConflict final {
    std::string id;
    std::string type;
    std::string name;
    std::string lineCode;
    std::string localContent;
    std::string remoteContent;
};

struct ManagedRecordSet final {
    std::vector<ManagedRecord> records;
    std::unordered_set<std::string> ids;
};

struct RemoteMergePlan final {
    std::vector<ManagedRecord> records;
    std::unordered_map<std::string, std::string> remoteIdsByLocalId;
    std::vector<RecordConflict> conflicts;
    bool changed{};
};

inline std::unordered_map<std::string, const service::dns::ProviderRecord*>
indexRemoteRecords(const std::vector<service::dns::ProviderRecord>& remoteRecords,
                   const std::vector<service::dns::ProviderLine>& lines) {
    if (remoteRecords.size() > kMaxInitialImportedRecords) {
        throw std::runtime_error("远程 DNS 记录超过单个域名可导入上限");
    }

    std::unordered_set<std::string> enabledLines;
    enabledLines.reserve(lines.size());
    for (const auto& line : lines) {
        if (!line.code.empty()) {
            enabledLines.emplace(line.code);
        }
    }

    std::unordered_map<std::string, const service::dns::ProviderRecord*> result;
    result.reserve(remoteRecords.size());
    for (const auto& record : remoteRecords) {
        validateRemoteRecord(record, enabledLines);
        if (!result.emplace(record.id, &record).second) {
            throw std::runtime_error("远程 DNS 记录 ID 重复");
        }
    }
    return result;
}

inline ManagedRecord toManagedRecord(const ZoneRecordData& record) {
    return {
        .id = record.id,
        .type = record.type,
        .name = record.name,
        .content = record.content,
        .ttl = record.ttl,
        .priority = record.priority,
        .proxied = record.proxied,
        .lineCode = record.lineCode,
    };
}

inline ManagedRecordSet buildManagedRecords(const ZoneConfigData& config) {
    ManagedRecordSet result;
    result.records.reserve(config.records.size());
    result.ids.reserve(config.records.size());
    for (const auto& record : config.records) {
        auto managed = toManagedRecord(record);
        if (!result.ids.emplace(managed.id).second) {
            throw std::runtime_error("DNS 聚合配置中的记录 ID 重复");
        }
        result.records.push_back(std::move(managed));
    }
    return result;
}

inline std::unordered_set<std::string>
collectDesiredConfigRecordIds(const ZoneConfigData& desiredConfig) {
    std::unordered_set<std::string> result;
    result.reserve(desiredConfig.records.size());
    for (const auto& record : desiredConfig.records) {
        result.emplace(record.id);
    }
    return result;
}

inline std::unordered_map<std::string, std::string>
collectRemoteIdsByLocalId(const ZoneRuntimeData& runtime) {
    std::unordered_map<std::string, std::string> result;
    result.reserve(runtime.recordStates.size());
    for (const auto& state : runtime.recordStates) {
        if (state.id && state.remoteRecordId) {
            result.emplace(*state.id, *state.remoteRecordId);
        }
    }
    return result;
}

inline RecordConflict makeRecordConflict(const ManagedRecord& local,
                                         std::string_view remoteContent) {
    return {
        .id = local.id,
        .type = local.type,
        .name = local.name,
        .lineCode = local.lineCode,
        .localContent = local.content,
        .remoteContent = std::string(remoteContent),
    };
}

inline const service::dns::ProviderRecord*
findUnclaimedRemoteRecord(const ManagedRecord& local,
                          const std::unordered_set<std::string>& claimedRemoteIds,
                          const std::vector<service::dns::ProviderRecord>& remoteRecords,
                          const service::dns::DnsProviderDriver& driver, std::string_view domain) {
    const auto identity = recordIdentity(toRemoteRecord(local, driver, domain));
    const service::dns::ProviderRecord* candidate = nullptr;
    for (const auto& remote : remoteRecords) {
        if (claimedRemoteIds.contains(remote.id) || recordIdentity(remote) != identity) {
            continue;
        }
        if (candidate) {
            throw std::runtime_error("远程 DNS 存在无法安全合并的同名记录");
        }
        candidate = &remote;
    }
    return candidate;
}

inline std::vector<RecordConflict> collectSyncConflicts(
    const std::vector<ManagedRecord>& localRecords,
    const std::unordered_map<std::string, std::string>& remoteIdsByLocalId,
    const std::unordered_set<std::string>& configIds,
    const std::unordered_map<std::string, const service::dns::ProviderRecord*>& remoteById,
    const std::vector<service::dns::ProviderRecord>& remoteRecords,
    const service::dns::DnsProviderDriver& driver, std::string_view domain) {
    std::unordered_set<std::string> claimedRemoteIds;
    claimedRemoteIds.reserve(remoteIdsByLocalId.size());
    for (const auto& [id, remoteId] : remoteIdsByLocalId) {
        if (configIds.contains(id) && remoteById.contains(remoteId)) {
            claimedRemoteIds.emplace(remoteId);
        }
    }

    std::vector<RecordConflict> conflicts;
    for (const auto& local : localRecords) {
        const auto known = remoteIdsByLocalId.find(local.id);
        if (known != remoteIdsByLocalId.end()) {
            const auto remote = remoteById.find(known->second);
            if (remote == remoteById.end()) {
                conflicts.push_back(makeRecordConflict(local, "（远端已删除）"));
            } else if (!recordMatches(local, *remote->second, driver, domain)) {
                conflicts.push_back(makeRecordConflict(local, remote->second->content));
            }
            continue;
        }

        const auto* candidate =
            findUnclaimedRemoteRecord(local, claimedRemoteIds, remoteRecords, driver, domain);
        if (candidate && !recordMatches(local, *candidate, driver, domain)) {
            conflicts.push_back(makeRecordConflict(local, candidate->content));
        }
    }
    return conflicts;
}

inline std::unordered_set<std::string> claimKnownRemoteIds(
    std::unordered_map<std::string, std::string>& remoteIdsByLocalId,
    const std::unordered_set<std::string>& configIds,
    const std::unordered_set<std::string>& desiredConfigIds,
    const std::unordered_map<std::string, const service::dns::ProviderRecord*>& remoteById,
    bool remotePreferred, bool& changed) {
    std::unordered_set<std::string> claimedRemoteIds;
    claimedRemoteIds.reserve(remoteIdsByLocalId.size());
    for (auto it = remoteIdsByLocalId.begin(); it != remoteIdsByLocalId.end();) {
        const auto remote = remoteById.find(it->second);
        if (remote == remoteById.end()) {
            ++it;
            continue;
        }
        if (!configIds.contains(it->first) && !desiredConfigIds.contains(it->first) &&
            remotePreferred) {
            it = remoteIdsByLocalId.erase(it);
            changed = true;
            continue;
        }
        claimedRemoteIds.emplace(it->second);
        ++it;
    }
    return claimedRemoteIds;
}

inline bool mergeManagedRecord(
    ManagedRecord& local, std::unordered_map<std::string, std::string>& remoteIdsByLocalId,
    std::unordered_set<std::string>& claimedRemoteIds,
    const std::unordered_map<std::string, const service::dns::ProviderRecord*>& remoteById,
    const std::vector<service::dns::ProviderRecord>& remoteRecords,
    const service::dns::DnsProviderDriver& driver, std::string_view domain, bool remotePreferred) {
    const auto known = remoteIdsByLocalId.find(local.id);
    if (known != remoteIdsByLocalId.end()) {
        const auto remote = remoteById.find(known->second);
        if (remote == remoteById.end()) {
            if (!remotePreferred) {
                return false;
            }
            local.id.clear();
            remoteIdsByLocalId.erase(known);
            return true;
        }
        if (remotePreferred && !recordMatches(local, *remote->second, driver, domain)) {
            applyRemoteRecord(local, *remote->second, driver, domain);
            return true;
        }
        return false;
    }

    const auto* candidate =
        findUnclaimedRemoteRecord(local, claimedRemoteIds, remoteRecords, driver, domain);
    if (!candidate) {
        return false;
    }
    remoteIdsByLocalId.emplace(local.id, candidate->id);
    claimedRemoteIds.emplace(candidate->id);
    if (remotePreferred && !recordMatches(local, *candidate, driver, domain)) {
        applyRemoteRecord(local, *candidate, driver, domain);
    }
    return true;
}

inline void appendUnclaimedRemoteRecords(
    std::vector<ManagedRecord>& merged, const std::unordered_set<std::string>& claimedRemoteIds,
    const std::vector<service::dns::ProviderRecord>& remoteRecords,
    const service::dns::DnsProviderDriver& driver, std::string_view domain, bool& changed) {
    std::erase_if(merged, [](const auto& record) { return record.id.empty(); });
    for (const auto& remote : remoteRecords) {
        if (claimedRemoteIds.contains(remote.id)) {
            continue;
        }
        merged.push_back(ManagedRecord{
            .id = {},
            .type = remote.type,
            .name = driver.localRecordName(remote.name, domain),
            .content = remote.content,
            .ttl = remote.ttl,
            .priority = remote.priority,
            .proxied = remote.proxied,
            .lineCode = remote.lineCode,
            .remoteId = remote.id,
        });
        changed = true;
    }
}

inline RemoteMergePlan
planRemoteMerge(std::string_view operation, const ZoneConfigData& config,
                const ZoneRuntimeData& runtime, const ZoneConfigData& desiredConfig,
                std::string_view domain, const service::dns::DnsProviderDriver& driver,
                const std::vector<service::dns::ProviderRecord>& remoteRecords,
                const std::vector<service::dns::ProviderLine>& lines) {
    const auto remoteById = indexRemoteRecords(remoteRecords, lines);
    auto managed = buildManagedRecords(config);
    const auto desiredConfigIds = collectDesiredConfigRecordIds(desiredConfig);
    auto remoteIdsByLocalId = collectRemoteIdsByLocalId(runtime);

    RemoteMergePlan result;
    if (operation == "sync") {
        result.conflicts = collectSyncConflicts(managed.records, remoteIdsByLocalId, managed.ids,
                                                remoteById, remoteRecords, driver, domain);
        if (!result.conflicts.empty()) {
            return result;
        }
    }

    result.changed = !runtime.recordsImported;
    const bool remotePreferred = operation == "sync_remote";
    auto claimedRemoteIds = claimKnownRemoteIds(remoteIdsByLocalId, managed.ids, desiredConfigIds,
                                                remoteById, remotePreferred, result.changed);
    for (auto& local : managed.records) {
        if (mergeManagedRecord(local, remoteIdsByLocalId, claimedRemoteIds, remoteById,
                               remoteRecords, driver, domain, remotePreferred)) {
            result.changed = true;
        }
    }
    appendUnclaimedRemoteRecords(managed.records, claimedRemoteIds, remoteRecords, driver, domain,
                                 result.changed);
    result.records = std::move(managed.records);
    result.remoteIdsByLocalId = std::move(remoteIdsByLocalId);
    return result;
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
    if (updated.affectedRows() != 0) {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        if (!co_await service::sync_runtime::completeRunning(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
    } else {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    }
    co_await transaction.commit();
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

inline ruvia::Task<bool>
mergeRemoteRecords(service::background::WorkerContext& context, const DnsTask& task,
                   const service::sync_runtime::RunningMarkerLease& lease,
                   std::int64_t configRevision, std::int64_t desiredRevision,
                   const ZoneConfigData& config, const ZoneRuntimeData& runtime,
                   const ZoneConfigData& desiredConfig, std::string_view domain,
                   const service::dns::DnsProviderDriver& driver,
                   const std::vector<service::dns::ProviderRecord>& remoteRecords,
                   const std::vector<service::dns::ProviderLine>& lines) {
    auto plan = planRemoteMerge(task.operation, config, runtime, desiredConfig, domain, driver,
                                remoteRecords, lines);
    if (!plan.conflicts.empty()) {
        co_await storeConflicts(context, task, lease, desiredRevision, runtime, lines,
                                plan.conflicts);
        co_return true;
    }
    if (!plan.changed) {
        co_return false;
    }
    co_return co_await persistRemoteMerge(context, task, lease, configRevision, desiredRevision,
                                          std::move(plan), runtime, lines);
}

struct ZoneSyncState final {
    ZoneRuntimeData runtime;
    ZoneConfigData config;
    ZoneConfigData desired;
    service::dns::DnsProviderConfigData providerConfig;
    std::string domain;
    std::string provider;
    std::string accountId;
    std::int64_t revision;
    std::int64_t configRevision;
    bool deleted;
};

struct RemoteZoneData final {
    std::string id;
    std::vector<service::dns::ProviderRecord> records;
    std::vector<service::dns::ProviderLine> lines;
};

struct DesiredRecordIds final {
    std::unordered_set<std::string> local;
    std::unordered_set<std::string> remote;
};

inline ruvia::Task<std::optional<ZoneSyncState>>
loadZoneSyncState(service::background::WorkerContext& context, const DnsTask& task,
                  const service::sync_runtime::RunningMarkerLease& lease) {
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    const auto rows = co_await context.db().query(
        "SELECT zone.desired_revision, zone.domain, zone.runtime::text, zone.deleted_at IS NOT "
        "NULL, provider.provider, provider.account_id, provider.config::text, provider.status, "
        "zone.config::text, zone.revision FROM "
        "sys_dns_zone zone INNER "
        "JOIN sys_provider provider ON provider.tenant_id = zone.tenant_id AND provider.id "
        "= zone.provider_id AND provider.kind = 'dns' WHERE zone.id = $1 AND zone.tenant_id = "
        "$2 AND provider.deleted_at IS NULL LIMIT 1",
        task.resourceId, task.tenantId);
    if (rows.empty()) {
        throw std::runtime_error("DNS 聚合根或服务商不存在");
    }
    const auto& row = rows.front();
    const auto revision = row[0].as<std::int64_t>().value_or(task.version);
    if (revision != task.version) {
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
        co_await transaction.commit();
        co_return std::nullopt;
    }

    const auto runtime =
        parseStoredRuntime(row[2].value().value_or("{}"), {.resource = context.resource()});
    if (!runtime) {
        throw std::runtime_error("DNS runtime 损坏");
    }
    const bool deleted = row[3].as<bool>().value_or(false) || task.operation == "delete";
    const std::string provider = std::string(row[4].value().value_or(""));
    const std::string accountId = std::string(row[5].value().value_or(""));
    auto providerConfig =
        service::dns::parseDnsProviderConfig(row[6].value().value_or("{}"), context.resource());
    if (row[7].value().value_or("") != "verified") {
        throw std::runtime_error("DNS 服务商账号尚未通过检测");
    }

    const std::optional<ZoneConfigData> config =
        parseStored(row[8].value().value_or("{}"), {.resource = context.resource()});
    if (!config) {
        throw std::runtime_error("DNS 聚合配置损坏");
    }

    ZoneConfigData desired = *config;
    const auto projected =
        co_await loadProjectedRecords(context.db(), task.tenantId, task.resourceId);
    for (const auto& record : projected) {
        desired.records.push_back({.id = record.id,
                                   .type = record.type,
                                   .name = record.name,
                                   .content = record.content,
                                   .ttl = record.ttl,
                                   .priority = record.priority,
                                   .proxied = record.proxied,
                                   .lineCode = record.lineCode});
    }
    for (const auto& challenge : runtime->challengeRecords) {
        desired.records.push_back({.id = challenge.id,
                                   .type = "TXT",
                                   .name = challenge.name,
                                   .content = challenge.content,
                                   .ttl = challenge.ttl,
                                   .priority = std::nullopt,
                                   .proxied = false,
                                   .lineCode = "default"});
    }

    co_return ZoneSyncState{
        .runtime = std::move(*runtime),
        .config = std::move(*config),
        .desired = std::move(desired),
        .providerConfig = std::move(providerConfig),
        .domain = std::string(row[1].value().value_or("")),
        .provider = provider,
        .accountId = accountId,
        .revision = revision,
        .configRevision = row[9].as<std::int64_t>().value_or(0),
        .deleted = deleted,
    };
}

inline ruvia::Task<RemoteZoneData>
loadRemoteZoneData(service::background::WorkerContext& context,
                   const service::sync_runtime::RunningMarkerLease& lease,
                   const service::dns::DnsProviderDriver& driver, std::string_view accountId,
                   std::string_view secret, std::string_view domain) {
    const auto remoteZone = co_await driver.findZone(context, accountId, secret, domain);
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    auto records = co_await driver.listRecords(context, accountId, secret, remoteZone.id, domain);
    if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
        throw std::runtime_error("DNS 同步标记 lease 已失效");
    }
    auto lines = co_await driver.listLines(context, accountId, secret, domain);
    co_return RemoteZoneData{
        .id = remoteZone.id,
        .records = std::move(records),
        .lines = std::move(lines),
    };
}

inline DesiredRecordIds
collectDesiredRecordIds(const ZoneConfigData& desiredConfig,
                        const std::unordered_map<std::string, std::string>& knownRemoteIds,
                        bool deleted) {
    DesiredRecordIds result;
    if (deleted) {
        return result;
    }
    result.local.reserve(desiredConfig.records.size());
    result.remote.reserve(desiredConfig.records.size());
    for (const auto& record : desiredConfig.records) {
        result.local.emplace(record.id);
        if (const auto known = knownRemoteIds.find(record.id); known != knownRemoteIds.end()) {
            result.remote.emplace(known->second);
        }
    }
    return result;
}

inline ruvia::Task<void> deleteObsoleteRemoteRecords(
    service::background::WorkerContext& context,
    const service::sync_runtime::RunningMarkerLease& lease,
    const service::dns::DnsProviderDriver& driver, std::string_view accountId,
    std::string_view secret, std::string_view remoteZoneId,
    const std::unordered_map<std::string, std::string>& knownRemoteIds,
    const DesiredRecordIds& desired, std::vector<service::dns::ProviderRecord>& remoteRecords) {
    for (const auto& [recordId, remoteId] : knownRemoteIds) {
        if (desired.local.contains(recordId) || desired.remote.contains(remoteId) ||
            remoteId.empty()) {
            continue;
        }
        const auto found = std::ranges::find_if(
            remoteRecords, [&](const auto& record) { return record.id == remoteId; });
        if (found == remoteRecords.end()) {
            continue;
        }
        if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        co_await driver.deleteRecord(context, accountId, secret, remoteZoneId, remoteId);
        std::erase_if(remoteRecords, [&](const auto& record) { return record.id == remoteId; });
    }
    co_return;
}

inline ruvia::Task<ZoneRuntimeDto> reconcileZoneRecords(
    service::background::WorkerContext& context,
    const service::sync_runtime::RunningMarkerLease& lease,
    const service::dns::DnsProviderDriver& driver, std::string_view accountId,
    std::string_view secret, std::string_view remoteZoneId, std::string_view domain,
    const ZoneConfigData& desiredConfig, const ZoneRuntimeData& currentRuntime,
    const std::unordered_map<std::string, std::string>& knownRemoteIds,
    std::vector<service::dns::ProviderRecord>& remoteRecords,
    const std::vector<service::dns::ProviderLine>& lines, std::int64_t revision, bool deleted) {
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
    if (!deleted) {
        for (const auto& record : desiredConfig.records) {
            if (!co_await service::sync_runtime::renewRunningLease(context.db(), lease)) {
                throw std::runtime_error("DNS 同步标记 lease 已失效");
            }
            const auto managed = toManagedRecord(record);
            const auto known = knownRemoteIds.find(managed.id);
            const auto remoteId = co_await driver.reconcileRecord(
                context, accountId, secret, remoteZoneId, domain,
                known == knownRemoteIds.end() ? std::string_view{}
                                              : std::string_view{known->second},
                managed.type, managed.name, managed.content, managed.ttl, managed.priority,
                managed.proxied, managed.lineCode, remoteRecords);
            auto& state = states.emplace_back(ruvia::ModelOptions{.resource = context.resource()});
            state.set<"id">(managed.id);
            state.set<"remoteRecordId">(remoteId);
            state.set<"syncStatus">("synced");
            state.set<"syncedRevision">(revision);

            auto resolved = toRemoteRecord(managed, driver, domain);
            resolved.id = remoteId;
            const auto existing = std::ranges::find_if(
                remoteRecords, [&](const auto& item) { return item.id == remoteId; });
            if (existing == remoteRecords.end()) {
                remoteRecords.push_back(std::move(resolved));
            } else {
                *existing = std::move(resolved);
            }
        }
    }
    appendChallengeRecords(nextRuntime, currentRuntime);
    co_return nextRuntime;
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
    if (updated.affectedRows() == 0) {
        (void)co_await service::sync_runtime::releaseRunning(transaction, lease);
    } else {
        if (!co_await service::sync_runtime::renewRunningLease(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
        if (task.operation == "delete") {
            if (!co_await service::sync_runtime::removeRunning(transaction, lease)) {
                throw std::runtime_error("DNS 删除同步标记 lease 已失效");
            }
        } else if (!co_await service::sync_runtime::completeRunning(transaction, lease)) {
            throw std::runtime_error("DNS 同步标记 lease 已失效");
        }
    }
    co_await transaction.commit();
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

inline ruvia::Task<void> reconcileTasks(service::background::WorkerContext& context) {
    const auto zones = co_await context.db().query(
        "SELECT tenant_id, id FROM sys_dns_zone WHERE deleted_at IS NULL ORDER BY sort ASC LIMIT "
        "128");
    for (const auto& zone : zones) {
        auto transaction = co_await context.db().beginTransaction();
        (void)co_await service::dns_sync::pruneClusterManagedRecords(
            transaction, std::string(zone[0].value().value_or("")),
            std::string(zone[1].value().value_or("")));
        co_await transaction.commit();
    }
    const auto missing = co_await context.db().query(
        "SELECT zone.tenant_id, zone.id, zone.desired_revision FROM sys_dns_zone zone WHERE "
        "zone.deleted_at IS NULL AND zone.synced_revision < zone.desired_revision AND NOT EXISTS "
        "(SELECT 1 FROM sys_sync_task task WHERE task.resource_type = 'dns_zone' AND "
        "task.tenant_id = zone.tenant_id AND task.resource_id = zone.id AND "
        "task.version = zone.desired_revision) ORDER BY zone.sort ASC LIMIT 32");
    for (const auto& row : missing) {
        auto transaction = co_await context.db().beginTransaction();
        const auto revision = row[2].as<std::int64_t>().value_or(1);
        const auto locked = co_await transaction.query(
            "SELECT 1 FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 AND "
            "desired_revision = $3 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            row[0].value().value_or(""), row[1].value().value_or(""), revision);
        if (!locked.empty()) {
            co_await service::dns_sync::enqueueZoneRevision(
                transaction, std::string(row[0].value().value_or("")),
                std::string(row[1].value().value_or("")), revision);
        }
        co_await transaction.commit();
    }
    co_return;
}

inline ruvia::Task<void> recoverStaleTasks(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(context.db(), "dns_zone");
    co_return;
}

inline ruvia::Task<std::optional<DnsTask>> claim(service::background::WorkerContext& context) {
    const auto rows = co_await context.db().query(
        "WITH candidate AS (SELECT task.id FROM sys_sync_task task INNER JOIN sys_dns_zone zone ON "
        "zone.tenant_id = task.tenant_id AND zone.id = task.resource_id WHERE "
        "task.resource_type = 'dns_zone' AND NOT task.is_done AND task.next_attempt_at <= NOW() "
        "AND task.lease_until IS NULL AND task.version = zone.desired_revision AND "
        "(zone.deleted_at IS NULL OR task.operation = 'delete') ORDER BY task.next_attempt_at ASC, "
        "task.updated_at ASC FOR UPDATE OF task SKIP LOCKED LIMIT 1) UPDATE sys_sync_task task "
        "SET lease_owner = $1, lease_until = NOW() + INTERVAL '60 seconds', updated_at = NOW() "
        "FROM candidate WHERE task.id = candidate.id RETURNING task.id, task.tenant_id, "
        "task.resource_id, task.operation, task.version, task.count_fails",
        context.leaseOwner());
    if (rows.empty()) {
        co_return std::nullopt;
    }
    const auto& row = rows.front();
    co_return DnsTask{
        .id = std::string(row[0].value().value_or("")),
        .tenantId = std::string(row[1].value().value_or("")),
        .resourceId = std::string(row[2].value().value_or("")),
        .operation = std::string(row[3].value().value_or("sync")),
        .version = row[4].as<std::int64_t>().value_or(1),
        .failures = row[5].as<std::int64_t>().value_or(0),
    };
}

inline ruvia::Task<std::int64_t> syncZone(service::background::WorkerContext& context,
                                          const DnsTask& task) {
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.version},
        .owner = context.leaseOwner(),
    };
    const auto state = co_await loadZoneSyncState(context, task, lease);
    if (!state) {
        co_return task.version;
    }
    service::utils::SensitiveString secret(
        service::utils::openSecret(state->providerConfig.credentialEnvelope));
    const service::dns::DnsProviderDriver driver(state->provider);
    for (const auto& record : state->desired.records) {
        if (record.ttl < driver.minimumRecordTtl()) {
            throw std::runtime_error("DNS 配置中的 TTL 低于当前服务商允许的最小值");
        }
    }

    RemoteZoneData remote;
    bool remoteZoneMissing = false;
    try {
        remote = co_await loadRemoteZoneData(context, lease, driver, state->accountId,
                                             secret.view(), state->domain);
    } catch (const service::dns::CloudflareError& error) {
        if (task.operation == "delete" &&
            error.code() == service::dns::CloudflareErrorCode::zoneNotFound) {
            remoteZoneMissing = true;
        } else {
            throw;
        }
    } catch (const service::dns::AliyunError& error) {
        if (task.operation == "delete" &&
            error.code() == service::dns::AliyunErrorCode::domainNotFound) {
            remoteZoneMissing = true;
        } else {
            throw;
        }
    }
    if (remoteZoneMissing) {
        co_await finishDeletedZoneWithoutRemote(context, task, lease);
        co_return task.version;
    }
    const bool imported = state->runtime.recordsImported;
    if (!state->deleted && !imported && state->config.records.empty()) {
        co_await importInitialRemoteRecords(context, task, lease, state->configRevision,
                                            state->domain, state->runtime, driver, remote.records,
                                            remote.lines);
        co_return task.version;
    }
    if (!state->deleted &&
        co_await mergeRemoteRecords(context, task, lease, state->configRevision, state->revision,
                                    state->config, state->runtime, state->desired, state->domain,
                                    driver, remote.records, remote.lines)) {
        co_return task.version;
    }

    const auto knownRemoteIds = collectRemoteIdsByLocalId(state->runtime);
    const auto desired = collectDesiredRecordIds(state->desired, knownRemoteIds, state->deleted);
    co_await deleteObsoleteRemoteRecords(context, lease, driver, state->accountId, secret.view(),
                                         remote.id, knownRemoteIds, desired, remote.records);
    const auto nextRuntime = co_await reconcileZoneRecords(
        context, lease, driver, state->accountId, secret.view(), remote.id, state->domain,
        state->desired, state->runtime, knownRemoteIds, remote.records, remote.lines,
        state->revision, state->deleted);
    co_await persistSyncedZone(context, task, lease, nextRuntime, state->revision);
    co_return state->revision;
}

inline ruvia::Task<void> fail(service::background::WorkerContext& context, const DnsTask& task,
                              std::string_view error, bool permanent) {
    const auto message = boundedError(error);
    const service::sync_runtime::RunningMarkerLease lease{
        .marker = {.tenantId = task.tenantId, .markerId = task.id, .version = task.version},
        .owner = context.leaseOwner(),
    };
    auto transaction = co_await context.db().beginTransaction();
    (void)co_await transaction.query(
        "SELECT id FROM sys_dns_zone WHERE tenant_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        task.tenantId, task.resourceId);
    const bool transitioned =
        co_await service::sync_runtime::failRunning(transaction, lease, message);
    if (transitioned) {
        (void)co_await transaction.execute(
            "UPDATE sys_dns_zone SET sync_status = $2, last_error = $3, updated_at = NOW() WHERE "
            "id = $1 AND desired_revision = $4 AND tenant_id = $5",
            task.resourceId, permanent ? std::string_view{"failed"} : std::string_view{"pending"},
            std::string_view(message), task.version, task.tenantId);
    }
    co_await transaction.commit();
    if (transitioned) {
        service::logging::error("DNS sync task " + task.id + " failed: " + message);
    }
    co_return;
}

struct TaskFailure final {
    std::string message;
    bool permanent{};
};

inline TaskFailure classifyTaskFailure(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        if (const auto* cloudflare = dynamic_cast<const service::dns::CloudflareError*>(&error)) {
            return {
                .message = boundedError(cloudflare->what()),
                .permanent =
                    cloudflare->code() == service::dns::CloudflareErrorCode::authorizationFailed ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::credentialInvalid ||
                    cloudflare->code() == service::dns::CloudflareErrorCode::recordConflict,
            };
        }
        if (const auto* aliyun = dynamic_cast<const service::dns::AliyunError*>(&error)) {
            return {
                .message = boundedError(aliyun->what()),
                .permanent = aliyun->code() == service::dns::AliyunErrorCode::authorizationFailed ||
                             aliyun->code() == service::dns::AliyunErrorCode::credentialInvalid ||
                             aliyun->code() == service::dns::AliyunErrorCode::recordConflict ||
                             aliyun->code() == service::dns::AliyunErrorCode::domainNotFound,
            };
        }
        return {.message = boundedError(error.what()), .permanent = false};
    } catch (...) {
        return {.message = "同步任务发生未知错误", .permanent = false};
    }
}

inline ruvia::Task<void> processTask(service::background::WorkerContext& context,
                                     const DnsTask& task) {
    std::exception_ptr exception;
    try {
        (void)co_await syncZone(context, task);
    } catch (...) {
        exception = std::current_exception();
    }
    if (exception) {
        const auto failure = classifyTaskFailure(exception);
        co_await fail(context, task, failure.message, failure.permanent);
    }
    co_return;
}

inline ruvia::Task<void> processTasks(service::background::WorkerContext& context,
                                      std::size_t& processed) {
    for (; processed < kMaxJobsPerTick; ++processed) {
        const auto task = co_await claim(context);
        if (!task) {
            break;
        }
        co_await processTask(context, *task);
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    auto nextLeaseRecovery = std::chrono::steady_clock::now();
    auto nextReconciliation = std::chrono::steady_clock::now();
    while (!context.stopToken().stopRequested()) {
        std::string workerError;
        std::size_t processed = 0;
        try {
            if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
                co_await recoverStaleTasks(context);
                nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
            }
            if (std::chrono::steady_clock::now() >= nextReconciliation) {
                co_await reconcileTasks(context);
                nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
            }
            co_await processTasks(context, processed);
        } catch (const std::exception& error) {
            workerError = boundedError(error.what());
        } catch (...) {
            workerError = "未知 DNS 同步错误";
        }
        if (!workerError.empty()) {
            service::logging::error("DNS sync worker failure: " + workerError);
        }
        const bool shouldIdle = processed == 0 || !workerError.empty();
        if (shouldIdle &&
            co_await ruvia::sleepFor(context.worker(), kIdlePollInterval, context.stopToken()) ==
                ruvia::TimerSleepResult::kStopRequested) {
            break;
        }
    }
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::dns_sync
