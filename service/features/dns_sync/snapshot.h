#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::dns_sync {

RUVIA_REQUEST_MODEL(ZoneRecordInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(proxied, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_REQUEST_MODEL(ZoneConfigInput, RUVIA_OPTIONAL_FIELD(records, ruvia::Array<ZoneRecordInput>));

RUVIA_RESPONSE_MODEL(ZoneRecordOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD(proxied, ruvia::Bool),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));
RUVIA_RESPONSE_MODEL(ZoneConfigOutput,
                     RUVIA_REQUIRED_FIELD(records, ruvia::Array<ZoneRecordOutput>));

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

[[nodiscard]] inline std::optional<ZoneRecordData> normalize(const ZoneRecordInput& input) {
    const auto& id = input.get<"id">();
    const auto& type = input.get<"type">();
    const auto& name = input.get<"name">();
    const auto& content = input.get<"content">();
    const auto& ttl = input.get<"ttl">();
    const auto& priority = input.get<"priority">();
    const auto& proxied = input.get<"proxied">();
    const auto& lineCode = input.get<"lineCode">();
    if (!id || !type || !name || !content || !ttl || !proxied || !lineCode) {
        return std::nullopt;
    }
    return ZoneRecordData{.id = std::string(id->view()),
                          .type = std::string(type->view()),
                          .name = std::string(name->view()),
                          .content = std::string(content->view()),
                          .ttl = ttl->value,
                          .priority = priority ? std::optional<std::int64_t>{priority->value}
                                               : std::nullopt,
                          .proxied = proxied->value,
                          .lineCode = std::string(lineCode->view())};
}

[[nodiscard]] inline std::optional<ZoneConfigData> normalize(const ZoneConfigInput& input) {
    const auto& records = input.get<"records">();
    if (!records) {
        return std::nullopt;
    }

    ZoneConfigData result;
    result.records.reserve(records->size());
    for (const auto& record : *records) {
        auto normalized = normalize(record);
        if (!normalized) {
            return std::nullopt;
        }
        result.records.push_back(std::move(*normalized));
    }
    return result;
}

[[nodiscard]] inline std::optional<ZoneConfigData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<ZoneConfigInput> input = ruvia::fromJson<ZoneConfigInput>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline ZoneConfigOutput toOutput(const ZoneConfigData& input,
                                               ruvia::ModelOptions options = {}) {
    ZoneConfigOutput output(options);
    auto& records = output.ensure<"records">();
    records.reserve(input.records.size());
    for (const auto& record : input.records) {
        auto& item = records.emplace_back(options);
        item.set<"id">(record.id);
        item.set<"type">(record.type);
        item.set<"name">(record.name);
        item.set<"content">(record.content);
        item.set<"ttl">(record.ttl);
        item.set<"proxied">(record.proxied);
        item.set<"lineCode">(record.lineCode);
        if (record.priority) {
            item.set<"priority">(*record.priority);
        }
    }
    return output;
}

RUVIA_REQUEST_MODEL(ZoneLineRuntimeInput, RUVIA_OPTIONAL_FIELD(code, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("display_name", displayName, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));
RUVIA_REQUEST_MODEL(ZoneRecordRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));
RUVIA_REQUEST_MODEL(ZoneRecordConflictInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("local_content", localContent, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_content", remoteContent, ruvia::String));
RUVIA_REQUEST_MODEL(ZoneChallengeRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("certificate_id", certificateId, ruvia::String));
RUVIA_REQUEST_MODEL(ZoneRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(lines, ruvia::Array<ZoneLineRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("record_states", recordStates,
                                              ruvia::Array<ZoneRecordRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD(conflicts, ruvia::Array<ZoneRecordConflictInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challengeRecords,
                                              ruvia::Array<ZoneChallengeRuntimeInput>));

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

[[nodiscard]] inline ZoneRuntimeData normalize(const ZoneRuntimeInput& input) {
    ZoneRuntimeData result;
    if (const auto& value = input.get<"recordsImported">()) {
        result.recordsImported = value->value;
    }
    if (const auto& value = input.get<"linesSyncedAt">()) {
        result.linesSyncedAt = std::string(value->view());
    }
    if (const auto& source = input.get<"lines">()) {
        result.lines.reserve(source->size());
        for (const auto& line : *source) {
            ZoneLineRuntimeData item;
            if (const auto& value = line.get<"code">()) {
                item.code = std::string(value->view());
            }
            if (const auto& value = line.get<"name">()) {
                item.name = std::string(value->view());
            }
            if (const auto& value = line.get<"displayName">()) {
                item.displayName = std::string(value->view());
            }
            if (const auto& value = line.get<"status">()) {
                item.status = std::string(value->view());
            }
            result.lines.push_back(std::move(item));
        }
    }
    if (const auto& source = input.get<"recordStates">()) {
        result.recordStates.reserve(source->size());
        for (const auto& state : *source) {
            ZoneRecordRuntimeData item;
            if (const auto& value = state.get<"id">()) {
                item.id = std::string(value->view());
            }
            if (const auto& value = state.get<"remoteRecordId">()) {
                item.remoteRecordId = std::string(value->view());
            }
            if (const auto& value = state.get<"syncStatus">()) {
                item.syncStatus = std::string(value->view());
            }
            if (const auto& value = state.get<"syncedRevision">()) {
                item.syncedRevision = value->value;
            }
            if (const auto& value = state.get<"lastError">()) {
                item.lastError = std::string(value->view());
            }
            result.recordStates.push_back(std::move(item));
        }
    }
    if (const auto& source = input.get<"conflicts">()) {
        result.conflicts.reserve(source->size());
        for (const auto& conflict : *source) {
            ZoneRecordConflictData item;
            if (const auto& value = conflict.get<"id">()) {
                item.id = std::string(value->view());
            }
            if (const auto& value = conflict.get<"type">()) {
                item.type = std::string(value->view());
            }
            if (const auto& value = conflict.get<"name">()) {
                item.name = std::string(value->view());
            }
            if (const auto& value = conflict.get<"lineCode">()) {
                item.lineCode = std::string(value->view());
            }
            if (const auto& value = conflict.get<"localContent">()) {
                item.localContent = std::string(value->view());
            }
            if (const auto& value = conflict.get<"remoteContent">()) {
                item.remoteContent = std::string(value->view());
            }
            result.conflicts.push_back(std::move(item));
        }
    }
    if (const auto& source = input.get<"challengeRecords">()) {
        result.challengeRecords.reserve(source->size());
        for (const auto& challenge : *source) {
            const auto& id = challenge.get<"id">();
            const auto& name = challenge.get<"name">();
            const auto& content = challenge.get<"content">();
            const auto& ttl = challenge.get<"ttl">();
            const auto& certificateId = challenge.get<"certificateId">();
            if (!id || !name || !content || !ttl || !certificateId) {
                continue;
            }
            result.challengeRecords.push_back(
                {.id = std::string(id->view()),
                 .name = std::string(name->view()),
                 .content = std::string(content->view()),
                 .ttl = ttl->value,
                 .certificateId = std::string(certificateId->view())});
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<ZoneRuntimeData>
parseStoredRuntime(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<ZoneRuntimeInput> input = ruvia::fromJson<ZoneRuntimeInput>(json, options);
    if (!input || !input->get<"recordsImported">() || !input->get<"lines">() ||
        !input->get<"recordStates">() || !input->get<"conflicts">()) {
        return std::nullopt;
    }
    return normalize(*input);
}

RUVIA_RESPONSE_MODEL(ZoneLineRuntimeDto, RUVIA_REQUIRED_FIELD(code, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("display_name", displayName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));
RUVIA_RESPONSE_MODEL(ZoneRecordRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(ZoneRecordConflictDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("local_content", localContent, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("remote_content", remoteContent, ruvia::String));
RUVIA_RESPONSE_MODEL(ZoneChallengeRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("certificate_id", certificateId, ruvia::String));
RUVIA_RESPONSE_MODEL(
    ZoneRuntimeDto, RUVIA_REQUIRED_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(lines, ruvia::Array<ZoneLineRuntimeDto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", recordStates, ruvia::Array<ZoneRecordRuntimeDto>),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::Array<ZoneRecordConflictDto>),
    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challengeRecords,
                              ruvia::Array<ZoneChallengeRuntimeDto>, RUVIA_OMIT_EMPTY));

inline void appendChallengeRecords(ZoneRuntimeDto& output, const ZoneRuntimeData& runtime) {
    if (runtime.challengeRecords.empty()) {
        return;
    }
    auto& records = output.ensure<"challengeRecords">();
    for (const auto& challenge : runtime.challengeRecords) {
        auto& item = records.emplace_back();
        item.set<"id">(challenge.id);
        item.set<"name">(challenge.name);
        item.set<"content">(challenge.content);
        item.set<"ttl">(challenge.ttl);
        item.set<"certificateId">(challenge.certificateId);
    }
}

[[nodiscard]] inline ZoneRuntimeDto toOutput(const ZoneRuntimeData& input,
                                             ruvia::ModelOptions options = {}) {
    ZoneRuntimeDto output(options);
    output.set<"recordsImported">(input.recordsImported);
    if (input.linesSyncedAt) {
        output.set<"linesSyncedAt">(*input.linesSyncedAt);
    }
    auto& lines = output.ensure<"lines">();
    for (const auto& line : input.lines) {
        auto& item = lines.emplace_back(options);
        item.set<"code">(line.code.value_or(""));
        item.set<"name">(line.name.value_or(""));
        item.set<"displayName">(line.displayName.value_or(""));
        item.set<"status">(line.status.value_or(""));
    }
    auto& states = output.ensure<"recordStates">();
    for (const auto& state : input.recordStates) {
        auto& item = states.emplace_back(options);
        item.set<"id">(state.id.value_or(""));
        item.set<"syncStatus">(state.syncStatus.value_or("pending"));
        item.set<"syncedRevision">(state.syncedRevision.value_or(0));
        if (state.remoteRecordId) {
            item.set<"remoteRecordId">(*state.remoteRecordId);
        }
        if (state.lastError) {
            item.set<"lastError">(*state.lastError);
        }
    }
    auto& conflicts = output.ensure<"conflicts">();
    for (const auto& conflict : input.conflicts) {
        auto& item = conflicts.emplace_back(options);
        item.set<"id">(conflict.id.value_or(""));
        item.set<"type">(conflict.type.value_or(""));
        item.set<"name">(conflict.name.value_or(""));
        item.set<"lineCode">(conflict.lineCode.value_or(""));
        item.set<"localContent">(conflict.localContent.value_or(""));
        item.set<"remoteContent">(conflict.remoteContent.value_or(""));
    }
    appendChallengeRecords(output, input);
    return output;
}

} // namespace service::dns_sync
