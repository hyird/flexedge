#pragma once
#include "common/cache_settings.h"
#include "service/features/cache_config/settings_transport.h"
namespace service::cache_config {
inline flexedge::cache::status_ttl from_dto(const status_ttl_dto& input) {
    flexedge::cache::status_ttl result;
    result.from_ = input.get<"from">().value_;
    result.to_ = input.get<"to">().value_;
    result.seconds_ = input.get<"seconds">().value_;
    return result;
}
inline status_ttl_dto to_dto(const flexedge::cache::status_ttl& input, ruvia::model_options options = {}) {
    status_ttl_dto result(options);
    result.set<"from">(input.from_);
    result.set<"to">(input.to_);
    result.set<"seconds">(input.seconds_);
    return result;
}
inline flexedge::cache::header_key from_dto(const header_key_dto& input) {
    flexedge::cache::header_key result;
    result.name_ = std::string(input.get<"name">().view());
    for (const auto& value : input.get<"values">()) {
        result.values_.push_back(std::string(value.view()));
    }
    return result;
}
inline header_key_dto to_dto(const flexedge::cache::header_key& input, ruvia::model_options options = {}) {
    header_key_dto result(options);
    result.set<"name">(input.name_);
    auto& values = result.ensure<"values">();
    for (const auto& value : input.values_) {
        values.emplace_back(value, options);
    }
    return result;
}
inline flexedge::cache::cache_key from_dto(const cache_key_dto& input) {
    flexedge::cache::cache_key result;
    result.query_mode_ = std::string(input.get<"query_mode">().view());
    for (const auto& value : input.get<"query_parameters">()) {
        result.query_parameters_.push_back(std::string(value.view()));
    }
    for (const auto& value : input.get<"headers">()) {
        result.headers_.push_back(std::string(value.view()));
    }
    for (const auto& value : input.get<"header_presence">()) {
        result.header_presence_.push_back(std::string(value.view()));
    }
    for (const auto& value : input.get<"header_values">()) {
        result.header_values_.push_back(from_dto(value));
    }
    for (const auto& value : input.get<"cookies">()) {
        result.cookies_.push_back(std::string(value.view()));
    }
    for (const auto& value : input.get<"cookie_presence">()) {
        result.cookie_presence_.push_back(std::string(value.view()));
    }
    result.include_origin_ = input.get<"include_origin">().value_;
    result.host_ = std::string(input.get<"host">().view());
    result.country_ = input.get<"country">().value_;
    result.language_ = input.get<"language">().value_;
    return result;
}
inline cache_key_dto to_dto(const flexedge::cache::cache_key& input, ruvia::model_options options = {}) {
    cache_key_dto result(options);
    result.set<"query_mode">(input.query_mode_);
    auto& query_parameters = result.ensure<"query_parameters">();
    for (const auto& value : input.query_parameters_) {
        query_parameters.emplace_back(value, options);
    }
    auto& headers = result.ensure<"headers">();
    for (const auto& value : input.headers_) {
        headers.emplace_back(value, options);
    }
    auto& header_presence = result.ensure<"header_presence">();
    for (const auto& value : input.header_presence_) {
        header_presence.emplace_back(value, options);
    }
    auto& header_values = result.ensure<"header_values">();
    for (const auto& value : input.header_values_) {
        header_values.emplace_back(to_dto(value, options));
    }
    auto& cookies = result.ensure<"cookies">();
    for (const auto& value : input.cookies_) {
        cookies.emplace_back(value, options);
    }
    auto& cookie_presence = result.ensure<"cookie_presence">();
    for (const auto& value : input.cookie_presence_) {
        cookie_presence.emplace_back(value, options);
    }
    result.set<"include_origin">(input.include_origin_);
    result.set<"host">(input.host_);
    result.set<"country">(input.country_);
    result.set<"language">(input.language_);
    return result;
}
inline flexedge::cache::vary_header from_dto(const vary_header_dto& input) {
    flexedge::cache::vary_header result;
    result.name_ = std::string(input.get<"name">().view());
    result.action_ = std::string(input.get<"action">().view());
    for (const auto& value : input.get<"values">()) {
        result.values_.push_back(std::string(value.view()));
    }
    return result;
}
inline vary_header_dto to_dto(const flexedge::cache::vary_header& input, ruvia::model_options options = {}) {
    vary_header_dto result(options);
    result.set<"name">(input.name_);
    result.set<"action">(input.action_);
    auto& values = result.ensure<"values">();
    for (const auto& value : input.values_) {
        values.emplace_back(value, options);
    }
    return result;
}
inline flexedge::cache::rule_settings from_dto(const rule_settings_dto& input) {
    flexedge::cache::rule_settings result;
    for (const auto& value : input.get<"enabled">()) {
        result.enabled_.push_back(std::string(value.view()));
    }
    for (const auto& value : input.get<"status_ttl">()) {
        result.status_ttl_.push_back(from_dto(value));
    }
    result.key_ = from_dto(input.get<"key">());
    result.armor_ = input.get<"armor">().value_;
    result.device_ = input.get<"device">().value_;
    result.sort_query_ = input.get<"sort_query">().value_;
    result.serve_stale_ = input.get<"serve_stale">().value_;
    result.strong_etags_ = input.get<"strong_etags">().value_;
    result.origin_control_ = input.get<"origin_control">().value_;
    result.vary_default_ = std::string(input.get<"vary_default">().view());
    for (const auto& value : input.get<"vary_headers">()) {
        result.vary_headers_.push_back(from_dto(value));
    }
    result.origin_range_ = std::string(input.get<"origin_range">().view());
    for (const auto& value : input.get<"ports">()) {
        result.ports_.push_back(value.value_);
    }
    result.read_timeout_ = input.get<"read_timeout">().value_;
    result.error_passthrough_ = input.get<"error_passthrough">().value_;
    return result;
}
inline rule_settings_dto to_dto(const flexedge::cache::rule_settings& input, ruvia::model_options options = {}) {
    rule_settings_dto result(options);
    auto& enabled = result.ensure<"enabled">();
    for (const auto& value : input.enabled_) {
        enabled.emplace_back(value, options);
    }
    auto& status_ttl = result.ensure<"status_ttl">();
    for (const auto& value : input.status_ttl_) {
        status_ttl.emplace_back(to_dto(value, options));
    }
    result.set<"key">(to_dto(input.key_, options));
    result.set<"armor">(input.armor_);
    result.set<"device">(input.device_);
    result.set<"sort_query">(input.sort_query_);
    result.set<"serve_stale">(input.serve_stale_);
    result.set<"strong_etags">(input.strong_etags_);
    result.set<"origin_control">(input.origin_control_);
    result.set<"vary_default">(input.vary_default_);
    auto& vary_headers = result.ensure<"vary_headers">();
    for (const auto& value : input.vary_headers_) {
        vary_headers.emplace_back(to_dto(value, options));
    }
    result.set<"origin_range">(input.origin_range_);
    auto& ports = result.ensure<"ports">();
    for (const auto& value : input.ports_) {
        ports.emplace_back(value);
    }
    result.set<"read_timeout">(input.read_timeout_);
    result.set<"error_passthrough">(input.error_passthrough_);
    return result;
}
}  // namespace service::cache_config
