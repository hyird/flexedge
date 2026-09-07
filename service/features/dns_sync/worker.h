#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/ModelJson.h>

#include "service/features/background/marker_worker_loop.h"
#include "service/features/background/worker_pool.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/failure.h"
#include "service/features/dns_sync/maintenance.h"
#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/snapshot.h"
#include "service/features/dns_sync/task.h"
#include "service/features/dns_sync/task_loader.h"
#include "service/features/dns_sync/zone_loader.h"
#include "service/features/dns_sync/zone_persistence.h"
#include "service/features/sync_runtime/error.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::dns_sync {

namespace detail {

inline constexpr std::chrono::seconds kIdlePollInterval{2};
inline constexpr std::chrono::seconds kLeaseRecoveryInterval{15};
inline constexpr std::chrono::minutes kReconciliationInterval{15};
inline constexpr std::size_t kMaxJobsPerTick{32};

inline ruvia::Task<bool>
mergeRemoteRecords(service::background::WorkerContext& context, const DnsTask& task,
                   const service::sync_runtime::RunningMarkerLease& lease,
                   std::int64_t configRevision, std::int64_t desiredRevision,
                   const ZoneConfigData& config, const ZoneRuntimeData& runtime,
                   const ZoneConfigData& desiredConfig, std::string_view domain,
                   const service::dns::DnsProviderDriver& driver,
                   const std::vector<service::dns::ProviderRecord>& remoteRecords,
                   const std::vector<service::dns::ProviderLine>& lines) {
    auto plan = planRemoteMerge(service::sync_runtime::markerOperationName(task.operation), config,
                                runtime, desiredConfig, domain, driver, remoteRecords, lines);
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

struct DesiredRecordIds final {
    std::unordered_set<std::string> local;
    std::unordered_set<std::string> remote;
};

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

inline ruvia::Task<void> recoverStaleTasks(service::background::WorkerContext& context) {
    co_await service::sync_runtime::recoverStaleRunning(
        context.db(), service::sync_runtime::MarkerResourceType::dnsZone);
    co_return;
}

inline ruvia::Task<std::int64_t> syncZone(service::background::WorkerContext& context,
                                          const DnsTask& task) {
    const auto lease = service::sync_runtime::makeRunningLease(task.tenantId, task.id, task.version,
                                                               context.leaseOwner());
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
        if (task.operation == service::sync_runtime::MarkerOperation::remove &&
            error.code() == service::dns::CloudflareErrorCode::zoneNotFound) {
            remoteZoneMissing = true;
        } else {
            throw;
        }
    } catch (const service::dns::AliyunError& error) {
        if (task.operation == service::sync_runtime::MarkerOperation::remove &&
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
        co_await failDnsTask(context, task, failure.message, failure.permanent);
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

inline ruvia::Task<void> runMaintenance(service::background::WorkerContext& context,
                                        std::chrono::steady_clock::time_point& nextLeaseRecovery,
                                        std::chrono::steady_clock::time_point& nextReconciliation) {
    if (std::chrono::steady_clock::now() >= nextLeaseRecovery) {
        co_await recoverStaleTasks(context);
        nextLeaseRecovery = std::chrono::steady_clock::now() + kLeaseRecoveryInterval;
    }
    if (std::chrono::steady_clock::now() >= nextReconciliation) {
        co_await reconcileTasks(context);
        nextReconciliation = std::chrono::steady_clock::now() + kReconciliationInterval;
    }
    co_return;
}

inline ruvia::Task<void> run(service::background::WorkerContext& context) {
    co_await service::background::runMarkerWorkerLoop(
        context, kIdlePollInterval, "DNS sync worker failure: ", "未知 DNS 同步错误",
        runMaintenance, processTasks, service::sync_runtime::boundedError);
    co_return;
}

} // namespace detail

inline ruvia::Task<void> runWorker(service::background::WorkerContext& context) {
    co_await detail::run(context);
}

} // namespace service::dns_sync
