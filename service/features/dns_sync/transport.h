#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

#include "service/common/uuid.h"

namespace service::dns_sync {

RUVIA_MODEL(ZoneRecordInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(proxied, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String));

RUVIA_MODEL(ZoneConfigInput, RUVIA_OPTIONAL_FIELD(records, ruvia::Array<ZoneRecordInput>));

[[nodiscard]] inline bool hasUniqueRecordIds(const ruvia::Array<ZoneRecordInput>& records) {
    std::unordered_set<std::string> ids;
    ids.reserve(records.size());
    for (const auto& record : records) {
        const auto& id = record.get<"id">();
        const auto parsedId = service::common::parseUuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        if (!parsedId || !ids.insert(*parsedId).second) return false;
    }
    return true;
}

inline void validateZoneConfig(const ZoneConfigInput& config, ruvia::Validator& validator) {
    const auto& records = config.get<"records">();
    if (!records) return;

    if (records->size() > 10000) {
        validator.add("records", "too_big", "单个域名最多保存10000条记录");
    }
    if (!hasUniqueRecordIds(*records)) {
        validator.add("records", "custom", "记录 ID 不能重复");
    }

    const auto validateRequiredString = [&validator](std::string_view path,
                                             std::string_view field, const auto& value,
                                             std::size_t maximum, std::string_view message) {
        const std::string fieldPath = std::string(path) + "." + std::string(field);
        if (!value || value->empty()) {
            validator.add(fieldPath, "required", message);
            return false;
        }
        if (value->size() > maximum) {
            validator.add(fieldPath, "too_big", message);
            return false;
        }
        return true;
    };

    for (std::size_t index = 0; index < records->size(); ++index) {
        const auto& record = (*records)[index];
        const std::string path = "records[" + std::to_string(index) + "]";
        const auto& id = record.get<"id">();
        if (validateRequiredString(path, "id", id, 36, "记录 ID 不正确") && id &&
            !service::common::parseUuid(std::optional<std::string_view>{id->view()})) {
            validator.add(path + ".id", "format", "记录 ID 不正确");
        }

        const auto& type = record.get<"type">();
        if (validateRequiredString(path, "type", type, 8, "记录类型不能为空") && type) {
            const auto value = type->view();
            if (value != "A" && value != "AAAA" && value != "CNAME" && value != "TXT" &&
                value != "MX") {
                validator.add(path + ".type", "enum", "记录类型不支持");
            }
        }

        (void)validateRequiredString(path, "name", record.get<"name">(), 253,
                                     "主机记录不能为空");
        (void)validateRequiredString(path, "content", record.get<"content">(), 4096,
                                     "记录值不能为空");
        if (const auto& ttl = record.get<"ttl">(); !ttl || ttl->value < 1 || ttl->value > 86400) {
            validator.add(path + ".ttl", "range", "TTL 不正确");
        }
        if (const auto& priority = record.get<"priority">();
            priority && (priority->value < 0 || priority->value > 65535)) {
            validator.add(path + ".priority", "range", "优先级不正确");
        }
        if (!record.get<"proxied">()) {
            validator.add(path + ".proxied", "required", "代理状态不能为空");
        }
        (void)validateRequiredString(path, "line_code", record.get<"lineCode">(), 64,
                                     "请选择 DNS 线路");
    }
}

RUVIA_MODEL(ZoneRecordOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD(priority, ruvia::Int64, RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD(proxied, ruvia::Bool),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String));

RUVIA_MODEL(ZoneConfigOutput,
                     RUVIA_REQUIRED_FIELD(records, ruvia::Array<ZoneRecordOutput>));

RUVIA_MODEL(ZoneLineRuntimeInput, RUVIA_OPTIONAL_FIELD(code, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("display_name", displayName, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));

RUVIA_MODEL(ZoneRecordRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String));

RUVIA_MODEL(ZoneRecordConflictInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(type, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("line_code", lineCode, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("local_content", localContent, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("remote_content", remoteContent, ruvia::String));

RUVIA_MODEL(ZoneChallengeRuntimeInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(content, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(ttl, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("certificate_id", certificateId, ruvia::String));

RUVIA_MODEL(ZoneRuntimeInput,
                    RUVIA_OPTIONAL_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
                    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(lines, ruvia::Array<ZoneLineRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("record_states", recordStates,
                                              ruvia::Array<ZoneRecordRuntimeInput>),
                    RUVIA_OPTIONAL_FIELD(conflicts, ruvia::Array<ZoneRecordConflictInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challengeRecords,
                                              ruvia::Array<ZoneChallengeRuntimeInput>));

RUVIA_MODEL(ZoneLineRuntimeDto, RUVIA_REQUIRED_FIELD(code, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("display_name", displayName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));

RUVIA_MODEL(ZoneRecordRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remoteRecordId, ruvia::String,
                                               RUVIA_OMIT_EMPTY),
                     RUVIA_REQUIRED_FIELD_NAME("sync_status", syncStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("synced_revision", syncedRevision, ruvia::Int64),
                     RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String,
                                               RUVIA_OMIT_EMPTY));

RUVIA_MODEL(ZoneRecordConflictDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(type, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("line_code", lineCode, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("local_content", localContent, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("remote_content", remoteContent, ruvia::String));

RUVIA_MODEL(ZoneChallengeRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(content, ruvia::String),
                     RUVIA_REQUIRED_FIELD(ttl, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("certificate_id", certificateId, ruvia::String));

RUVIA_MODEL(
    ZoneRuntimeDto, RUVIA_REQUIRED_FIELD_NAME("records_imported", recordsImported, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", linesSyncedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(lines, ruvia::Array<ZoneLineRuntimeDto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", recordStates, ruvia::Array<ZoneRecordRuntimeDto>),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::Array<ZoneRecordConflictDto>),
    RUVIA_REQUIRED_FIELD_NAME("challenge_records", challengeRecords,
                              ruvia::Array<ZoneChallengeRuntimeDto>));

} // namespace service::dns_sync
