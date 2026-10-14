#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "service/common/uuid.h"

namespace service::dns_sync {

RUVIA_MODEL(zone_record_input, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(content, ruvia::string),
    RUVIA_OPTIONAL_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(proxied, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("line_code", line_code, ruvia::string));

RUVIA_MODEL(zone_config_input, RUVIA_OPTIONAL_FIELD(records, ruvia::array<zone_record_input>));

[[nodiscard]] inline bool has_unique_record_ids(const ruvia::array<zone_record_input>& records) {
    std::unordered_set<std::string> ids;
    ids.reserve(records.size());
    for (const auto& record : records) {
        const auto& id = record.get<"id">();
        const auto parsed_id = service::common::parse_uuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        if (!parsed_id || !ids.insert(*parsed_id).second) {
            return false;
        }
    }
    return true;
}

inline void validate_zone_config(const zone_config_input& config, ruvia::validator& validator) {
    const auto& records = config.get<"records">();
    if (!records) {
        return;
    }

    if (records->size() > 10000) {
        validator.add("records", "too_big", "单个域名最多保存10000条记录");
    }
    if (!has_unique_record_ids(*records)) {
        validator.add("records", "custom", "记录 ID 不能重复");
    }

    const auto validate_required_string = [&validator](std::string_view path,
                                              std::string_view field, const auto& value,
                                              std::size_t maximum, std::string_view message) {
        const std::string field_path = std::string(path) + "." + std::string(field);
        if (!value || value->empty()) {
            validator.add(field_path, "required", message);
            return false;
        }
        if (value->size() > maximum) {
            validator.add(field_path, "too_big", message);
            return false;
        }
        return true;
    };

    for (std::size_t index = 0; index < records->size(); ++index) {
        const auto& record = (*records)[index];
        const std::string path = "records[" + std::to_string(index) + "]";
        const auto& id = record.get<"id">();
        if (validate_required_string(path, "id", id, 36, "记录 ID 不正确") && id &&
            !service::common::parse_uuid(std::optional<std::string_view>{id->view()})) {
            validator.add(path + ".id", "format", "记录 ID 不正确");
        }

        const auto& type = record.get<"type">();
        if (validate_required_string(path, "type", type, 8, "记录类型不能为空") && type) {
            const auto value = type->view();
            if (value != "A" && value != "AAAA" && value != "CNAME" && value != "TXT" &&
                value != "MX") {
                validator.add(path + ".type", "enum", "记录类型不支持");
            }
        }

        (void)validate_required_string(path, "name", record.get<"name">(), 253,
            "主机记录不能为空");
        (void)validate_required_string(path, "content", record.get<"content">(), 4096,
            "记录值不能为空");
        if (const auto& ttl = record.get<"ttl">(); !ttl || ttl->value_ < 1 || ttl->value_ > 86400) {
            validator.add(path + ".ttl", "range", "TTL 不正确");
        }
        if (const auto& priority = record.get<"priority">();
            priority && (priority->value_ < 0 || priority->value_ > 65535)) {
            validator.add(path + ".priority", "range", "优先级不正确");
        }
        if (!record.get<"proxied">()) {
            validator.add(path + ".proxied", "required", "代理状态不能为空");
        }
        (void)validate_required_string(path, "line_code", record.get<"line_code">(), 64,
            "请选择 DNS 线路");
    }
}

RUVIA_MODEL(zone_record_output, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(content, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD(priority, ruvia::int64, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(proxied, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD_NAME("line_code", line_code, ruvia::string));

RUVIA_MODEL(zone_config_output,
    RUVIA_REQUIRED_FIELD(records, ruvia::array<zone_record_output>));

RUVIA_MODEL(zone_line_runtime_input, RUVIA_OPTIONAL_FIELD(code, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("display_name", display_name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string));

RUVIA_MODEL(zone_record_runtime_input, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remote_record_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("sync_status", sync_status, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("synced_revision", synced_revision, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string));

RUVIA_MODEL(zone_record_conflict_input, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(type, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("line_code", line_code, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("local_content", local_content, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("remote_content", remote_content, ruvia::string));

RUVIA_MODEL(zone_challenge_runtime_input, RUVIA_OPTIONAL_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(content, ruvia::string),
    RUVIA_OPTIONAL_FIELD(ttl, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("certificate_id", certificate_id, ruvia::string));

RUVIA_MODEL(zone_runtime_input,
    RUVIA_OPTIONAL_FIELD_NAME("records_imported", records_imported, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", lines_synced_at, ruvia::string),
    RUVIA_OPTIONAL_FIELD(lines, ruvia::array<zone_line_runtime_input>),
    RUVIA_OPTIONAL_FIELD_NAME("record_states", record_states,
        ruvia::array<zone_record_runtime_input>),
    RUVIA_OPTIONAL_FIELD(conflicts, ruvia::array<zone_record_conflict_input>),
    RUVIA_OPTIONAL_FIELD_NAME("challenge_records", challenge_records,
        ruvia::array<zone_challenge_runtime_input>));

RUVIA_MODEL(zone_line_runtime_dto, RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("display_name", display_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string));

RUVIA_MODEL(zone_record_runtime_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("remote_record_id", remote_record_id, ruvia::string,
        RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("sync_status", sync_status, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("synced_revision", synced_revision, ruvia::int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", last_error, ruvia::string,
        RUVIA_OMIT_EMPTY));

RUVIA_MODEL(zone_record_conflict_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("line_code", line_code, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("local_content", local_content, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("remote_content", remote_content, ruvia::string));

RUVIA_MODEL(zone_challenge_runtime_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(content, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("certificate_id", certificate_id, ruvia::string));

RUVIA_MODEL(
    zone_runtime_dto, RUVIA_REQUIRED_FIELD_NAME("records_imported", records_imported, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD_NAME("lines_synced_at", lines_synced_at, ruvia::string, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(lines, ruvia::array<zone_line_runtime_dto>),
    RUVIA_REQUIRED_FIELD_NAME("record_states", record_states, ruvia::array<zone_record_runtime_dto>),
    RUVIA_REQUIRED_FIELD(conflicts, ruvia::array<zone_record_conflict_dto>),
    RUVIA_REQUIRED_FIELD_NAME("challenge_records", challenge_records,
        ruvia::array<zone_challenge_runtime_dto>));

}  // namespace service::dns_sync
