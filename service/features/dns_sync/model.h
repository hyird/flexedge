#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::dns_sync {

struct ZoneRecordData final {
    std::string id;
    std::string type;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::optional<std::int64_t> priority;
    bool proxied;
    std::string lineCode;
};

struct ZoneConfigData final {
    std::vector<ZoneRecordData> records;
};

struct ZoneLineRuntimeData final {
    std::optional<std::string> code;
    std::optional<std::string> name;
    std::optional<std::string> displayName;
    std::optional<std::string> status;
};

struct ZoneRecordRuntimeData final {
    std::optional<std::string> id;
    std::optional<std::string> remoteRecordId;
    std::optional<std::string> syncStatus;
    std::optional<std::int64_t> syncedRevision;
    std::optional<std::string> lastError;
};

struct ZoneRecordConflictData final {
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<std::string> name;
    std::optional<std::string> lineCode;
    std::optional<std::string> localContent;
    std::optional<std::string> remoteContent;
};

struct ZoneChallengeRuntimeData final {
    std::string id;
    std::string name;
    std::string content;
    std::int64_t ttl;
    std::string certificateId;
};

struct ZoneRuntimeData final {
    bool recordsImported{false};
    std::optional<std::string> linesSyncedAt;
    std::vector<ZoneLineRuntimeData> lines;
    std::vector<ZoneRecordRuntimeData> recordStates;
    std::vector<ZoneRecordConflictData> conflicts;
    std::vector<ZoneChallengeRuntimeData> challengeRecords;
};

} // namespace service::dns_sync
