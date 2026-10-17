#pragma once
#include <ruvia/web/model_json.h>

#include "common/cache_policy.h"
#include "service/features/cache_config/settings_mapper.h"
#include "service/features/cache_config/transport.h"
namespace service::cache_config {
inline flexedge::cache::rule from_dto(const rule_dto& input) {
    flexedge::cache::rule result;
    result.id_ = std::string(input.get<"id">().view());
    result.name_ = std::string(input.get<"name">().view());
    result.enabled_ = input.get<"enabled">().value_;
    result.match_type_ = std::string(input.get<"match_type">().view());
    result.patterns_.clear();
    for (const auto& value : input.get<"patterns">()) {
        result.patterns_.push_back(std::string(value.view()));
    }
    result.action_ = std::string(input.get<"action">().view());
    result.ttl_mode_ = std::string(input.get<"ttl_mode">().view());
    result.ttl_seconds_ = input.get<"ttl_seconds">().value_;
    result.ignore_query_ = input.get<"ignore_query">().value_;
    result.methods_.clear();
    for (const auto& value : input.get<"methods">()) {
        result.methods_.push_back(std::string(value.view()));
    }
    result.status_codes_.clear();
    for (const auto& value : input.get<"status_codes">()) {
        result.status_codes_.push_back(value.value_);
    }
    result.min_object_bytes_ = input.get<"min_object_bytes">().value_;
    result.max_object_bytes_ = input.get<"max_object_bytes">().value_;
    result.skip_set_cookie_ = input.get<"skip_set_cookie">().value_;
    result.browser_ttl_seconds_ = input.get<"browser_ttl_seconds">().value_;
    result.skip_no_cache_ = input.get<"skip_no_cache">() ? input.get<"skip_no_cache">()->value_ : true;
    result.respect_request_no_cache_ = input.get<"respect_request_no_cache">() ? input.get<"respect_request_no_cache">()->value_ : false;
    result.skip_request_cookie_ = input.get<"skip_request_cookie">() ? input.get<"skip_request_cookie">()->value_ : false;
    result.browser_ttl_mode_ = input.get<"browser_ttl_mode">() ? std::string(input.get<"browser_ttl_mode">()->view()) : result.browser_ttl_seconds_ > 0 ? "override_origin"
                                                                                                                                                        : "default";
    if (input.get<"settings">()) {
        result.settings_ = from_dto(*input.get<"settings">());
    }
    return result;
}
inline flexedge::cache::policy from_dto(const policy_dto& input) {
    flexedge::cache::policy result;
    result.storage_ = std::string(input.get<"storage">().view());
    result.memory_bytes_ = input.get<"memory_bytes">().value_;
    result.disk_bytes_ = input.get<"disk_bytes">().value_;
    result.memory_percent_ = input.get<"memory_percent">() ? input.get<"memory_percent">()->value_ : 0;
    result.disk_percent_ = input.get<"disk_percent">() ? input.get<"disk_percent">()->value_ : 0;
    result.memory_reserved_bytes_ = input.get<"memory_reserved_bytes">() ? input.get<"memory_reserved_bytes">()->value_ : 0;
    result.disk_reserved_bytes_ = input.get<"disk_reserved_bytes">() ? input.get<"disk_reserved_bytes">()->value_ : 0;
    result.max_object_bytes_ = input.get<"max_object_bytes">().value_;
    result.directory_ = std::string(input.get<"directory">().view());
    for (const auto& value : input.get<"rules">()) {
        result.rules_.push_back(from_dto(value));
    }
    return result;
}
inline rule_dto to_dto(const flexedge::cache::rule& input, ruvia::model_options options = {}) {
    rule_dto result(options);
    result.set<"id">(input.id_);
    result.set<"name">(input.name_);
    result.set<"enabled">(input.enabled_);
    result.set<"match_type">(input.match_type_);
    (void)result.ensure<"patterns">();
    for (const auto& value : input.patterns_) {
        result.ensure<"patterns">().emplace_back(value, options);
    }
    result.set<"action">(input.action_);
    result.set<"ttl_mode">(input.ttl_mode_);
    result.set<"ttl_seconds">(input.ttl_seconds_);
    result.set<"ignore_query">(input.ignore_query_);
    for (const auto& value : input.methods_) {
        result.ensure<"methods">().emplace_back(value, options);
    }
    (void)result.ensure<"status_codes">();
    for (const auto& value : input.status_codes_) {
        result.ensure<"status_codes">().push_back(value);
    }
    result.set<"min_object_bytes">(input.min_object_bytes_);
    result.set<"max_object_bytes">(input.max_object_bytes_);
    result.set<"skip_set_cookie">(input.skip_set_cookie_);
    result.set<"browser_ttl_seconds">(input.browser_ttl_seconds_);
    result.set<"skip_no_cache">(input.skip_no_cache_);
    result.set<"respect_request_no_cache">(input.respect_request_no_cache_);
    result.set<"skip_request_cookie">(input.skip_request_cookie_);
    result.set<"browser_ttl_mode">(input.browser_ttl_mode_);
    if (input.settings_) {
        result.set<"settings">(to_dto(*input.settings_, options));
    }
    return result;
}
inline policy_dto to_dto(const flexedge::cache::policy& input, ruvia::model_options options = {}) {
    policy_dto result(options);
    result.set<"storage">(input.storage_);
    result.set<"memory_bytes">(input.memory_bytes_);
    result.set<"disk_bytes">(input.disk_bytes_);
    result.set<"memory_percent">(input.memory_percent_);
    result.set<"disk_percent">(input.disk_percent_);
    result.set<"memory_reserved_bytes">(input.memory_reserved_bytes_);
    result.set<"disk_reserved_bytes">(input.disk_reserved_bytes_);
    result.set<"max_object_bytes">(input.max_object_bytes_);
    result.set<"directory">(input.directory_);
    (void)result.ensure<"rules">();
    for (const auto& value : input.rules_) {
        result.ensure<"rules">().push_back(to_dto(value, options));
    }
    return result;
}
inline flexedge::cache::policy parse(std::string_view json) {
    const auto value = ruvia::from_json<policy_dto>(json);
    if (!value) {
        throw std::runtime_error("invalid stored cache policy");
    }
    auto result = from_dto(*value);
    if (!flexedge::cache::validate(result).empty()) {
        throw std::runtime_error("invalid stored cache policy");
    }
    return result;
}
}  // namespace service::cache_config
