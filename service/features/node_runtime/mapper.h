#pragma once

#include <ruvia/web/model_json.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/features/node_runtime/model.h"
#include "service/features/node_runtime/transport.h"

namespace service::node_runtime {

[[nodiscard]] inline node_runtime_data normalize(const node_runtime_input& input) {
    node_runtime_data result;
    if (const auto& value = input.get<"agent_version">()) {
        result.agent_version_ = std::string(value->view());
    }
    if (const auto& value = input.get<"cpu_usage">()) {
        result.cpu_usage_ = value->value_;
    }
    if (const auto& value = input.get<"memory_usage">()) {
        result.memory_usage_ = value->value_;
    }
    if (const auto& value = input.get<"traffic_out_bps">()) {
        result.traffic_out_bps_ = value->value_;
    }
    if (const auto& value = input.get<"connection_count">()) {
        result.connection_count_ = value->value_;
    }
    if (const auto& value = input.get<"load1m">()) {
        result.load1m_ = value->value_;
    }
    if (const auto& value = input.get<"queued_log_events">()) {
        result.queued_log_events_ = value->value_;
    }
    if (const auto& value = input.get<"dropped_log_events">()) {
        result.dropped_log_events_ = value->value_;
    }
    if (const auto& values = input.get<"origin_health">()) {
        result.origin_health_.reserve(values->size());
        for (const auto& value : *values) {
            const auto& website_id = value.get<"website_id">();
            const auto& origin_id = value.get<"origin_id">();
            const auto& status = value.get<"status">();
            const auto& checked_at_unix_millis = value.get<"checked_at_unix_millis">();
            const auto& latency_millis = value.get<"latency_millis">();
            if (!website_id || !origin_id || !status || !checked_at_unix_millis || !latency_millis) {
                continue;
            }
            node_runtime_data::origin_health item{.website_id_ = std::string(website_id->view()),
                .origin_id_ = std::string(origin_id->view()),
                .status_ = std::string(status->view()),
                .checked_at_unix_millis_ = checked_at_unix_millis->value_,
                .latency_millis_ = latency_millis->value_,
                .last_error_ = std::nullopt};
            if (const auto& last_error = value.get<"last_error">()) {
                item.last_error_ = std::string(last_error->view());
            }
            result.origin_health_.push_back(std::move(item));
        }
    }
    if (const auto& value = input.get<"health">()) {
        result.health_ = std::string(value->view());
    }
    if (const auto& value = input.get<"last_error">()) {
        result.last_error_ = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline std::optional<node_runtime_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<node_runtime_input> input = ruvia::from_json<node_runtime_input>(json, options);
    return input ? std::optional<node_runtime_data>{normalize(*input)} : std::nullopt;
}

}  // namespace service::node_runtime
