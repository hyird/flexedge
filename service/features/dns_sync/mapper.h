#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/dns_sync/model.h"
#include "service/features/dns_sync/transport.h"

namespace service::dns_sync {

[[nodiscard]] inline std::optional<zone_record_data> normalize(const zone_record_input& input) {
    const auto& id = input.get<"id">();
    const auto& type = input.get<"type">();
    const auto& name = input.get<"name">();
    const auto& content = input.get<"content">();
    const auto& ttl = input.get<"ttl">();
    const auto& priority = input.get<"priority">();
    const auto& proxied = input.get<"proxied">();
    const auto& line_code = input.get<"line_code">();
    if (!id || !type || !name || !content || !ttl || !proxied || !line_code) {
        return std::nullopt;
    }
    return zone_record_data{.id_ = std::string(id->view()),
        .type_ = std::string(type->view()),
        .name_ = std::string(name->view()),
        .content_ = std::string(content->view()),
        .ttl_ = ttl->value_,
        .priority_ = priority ? std::optional<std::int64_t>{priority->value_}
                              : std::nullopt,
        .proxied_ = proxied->value_,
        .line_code_ = std::string(line_code->view())};
}

[[nodiscard]] inline std::optional<zone_config_data> normalize(const zone_config_input& input) {
    const auto& records = input.get<"records">();
    if (!records) {
        return std::nullopt;
    }

    zone_config_data result;
    result.records_.reserve(records->size());
    for (const auto& record : *records) {
        auto normalized = normalize(record);
        if (!normalized) {
            return std::nullopt;
        }
        result.records_.push_back(std::move(*normalized));
    }
    return result;
}

[[nodiscard]] inline std::optional<zone_config_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<zone_config_input> input = ruvia::from_json<zone_config_input>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline zone_config_output to_output(const zone_config_data& input,
    ruvia::model_options options = {}) {
    zone_config_output output(options);
    auto& records = output.ensure<"records">();
    records.reserve(input.records_.size());
    for (const auto& record : input.records_) {
        auto& item = records.emplace_back(options);
        item.set<"id">(record.id_);
        item.set<"type">(record.type_);
        item.set<"name">(record.name_);
        item.set<"content">(record.content_);
        item.set<"ttl">(record.ttl_);
        item.set<"proxied">(record.proxied_);
        item.set<"line_code">(record.line_code_);
        if (record.priority_) {
            item.set<"priority">(*record.priority_);
        }
    }
    return output;
}

[[nodiscard]] inline zone_runtime_data normalize(const zone_runtime_input& input) {
    zone_runtime_data result;
    if (const auto& source = input.get<"lines">()) {
        result.lines_.reserve(source->size());
        for (const auto& line : *source) {
            zone_line_runtime_data item;
            if (const auto& value = line.get<"code">()) {
                item.code_ = std::string(value->view());
            }
            if (const auto& value = line.get<"name">()) {
                item.name_ = std::string(value->view());
            }
            if (const auto& value = line.get<"display_name">()) {
                item.display_name_ = std::string(value->view());
            }
            if (const auto& value = line.get<"status">()) {
                item.status_ = std::string(value->view());
            }
            result.lines_.push_back(std::move(item));
        }
    }
    if (const auto& source = input.get<"record_states">()) {
        result.record_states_.reserve(source->size());
        for (const auto& state : *source) {
            zone_record_runtime_data item;
            if (const auto& value = state.get<"id">()) {
                item.id_ = std::string(value->view());
            }
            if (const auto& value = state.get<"remote_record_id">()) {
                item.remote_record_id_ = std::string(value->view());
            }
            if (const auto& value = state.get<"sync_status">()) {
                item.sync_status_ = std::string(value->view());
            }
            if (const auto& value = state.get<"synced_revision">()) {
                item.synced_revision_ = value->value_;
            }
            if (const auto& value = state.get<"last_error">()) {
                item.last_error_ = std::string(value->view());
            }
            if (const auto& managed = state.get<"managed">()) {
                item.managed_ = managed->value_;
            }
            result.record_states_.push_back(std::move(item));
        }
    }
    if (const auto& source = input.get<"challenge_records">()) {
        result.challenge_records_.reserve(source->size());
        for (const auto& challenge : *source) {
            const auto& id = challenge.get<"id">();
            const auto& name = challenge.get<"name">();
            const auto& content = challenge.get<"content">();
            const auto& ttl = challenge.get<"ttl">();
            const auto& certificate_id = challenge.get<"certificate_id">();
            if (!id || !name || !content || !ttl || !certificate_id) {
                continue;
            }
            result.challenge_records_.push_back(
                {.id_ = std::string(id->view()),
                    .name_ = std::string(name->view()),
                    .content_ = std::string(content->view()),
                    .ttl_ = ttl->value_,
                    .certificate_id_ = std::string(certificate_id->view())});
        }
    }
    return result;
}

[[nodiscard]] inline std::optional<zone_runtime_data>
parse_stored_runtime(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<zone_runtime_input> input = ruvia::from_json<zone_runtime_input>(json, options);
    if (!input || !input->get<"lines">() || !input->get<"record_states">()) {
        return std::nullopt;
    }
    return normalize(*input);
}

inline void append_challenge_records(zone_runtime_dto& output, const zone_runtime_data& runtime) {
    if (runtime.challenge_records_.empty()) {
        return;
    }
    auto& records = output.ensure<"challenge_records">();
    for (const auto& challenge : runtime.challenge_records_) {
        auto& item = records.emplace_back();
        item.set<"id">(challenge.id_);
        item.set<"name">(challenge.name_);
        item.set<"content">(challenge.content_);
        item.set<"ttl">(challenge.ttl_);
        item.set<"certificate_id">(challenge.certificate_id_);
    }
}

[[nodiscard]] inline zone_runtime_dto to_output(const zone_runtime_data& input,
    ruvia::model_options options = {}) {
    zone_runtime_dto output(options);
    auto& lines = output.ensure<"lines">();
    for (const auto& line : input.lines_) {
        auto& item = lines.emplace_back(options);
        item.set<"code">(line.code_.value_or(""));
        item.set<"name">(line.name_.value_or(""));
        item.set<"display_name">(line.display_name_.value_or(""));
        item.set<"status">(line.status_.value_or(""));
    }
    auto& states = output.ensure<"record_states">();
    for (const auto& state : input.record_states_) {
        auto& item = states.emplace_back(options);
        item.set<"id">(state.id_.value_or(""));
        item.set<"sync_status">(state.sync_status_.value_or("pending"));
        item.set<"synced_revision">(state.synced_revision_.value_or(0));
        item.set<"managed">(state.managed_);
        if (state.remote_record_id_) {
            item.set<"remote_record_id">(*state.remote_record_id_);
        }
        if (state.last_error_) {
            item.set<"last_error">(*state.last_error_);
        }
    }
    append_challenge_records(output, input);
    return output;
}

}  // namespace service::dns_sync
