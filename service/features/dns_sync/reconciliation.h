#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "service/common/domain_name.h"
#include "service/features/dns/driver.h"
#include "service/features/dns_sync/snapshot.h"

namespace service::dns_sync::detail {

inline constexpr std::size_t kMaxReconciliationRecords{10'000};

inline bool supportsManagedRecordType(std::string_view type) {
    return type == "A" || type == "AAAA" || type == "CNAME" || type == "TXT" || type == "MX";
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
           expected.ttl == remote.ttl &&
           (expected.type != "MX" || expected.priority == remote.priority) &&
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

} // namespace service::dns_sync::detail
