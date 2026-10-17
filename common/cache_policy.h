#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/cache_settings_proto.h"
#include "common/request_expression.h"
#include "common/route_match.h"

namespace flexedge::cache {

inline constexpr std::int64_t maximum_object_bytes{512 * 1024 * 1024};

struct rule final {
    std::string id_{};
    std::string name_{};
    bool enabled_{true};
    std::string match_type_{"extension"};
    std::vector<std::string> patterns_{};
    std::string action_{"cache"};
    std::string ttl_mode_{"origin"};
    std::int64_t ttl_seconds_{86400};
    bool ignore_query_{false};
    std::vector<std::string> methods_{"GET", "HEAD"};
    std::vector<std::int64_t> status_codes_{};
    std::int64_t min_object_bytes_{0};
    std::int64_t max_object_bytes_{0};
    bool skip_set_cookie_{true};
    std::int64_t browser_ttl_seconds_{0};
    bool skip_no_cache_{true};
    bool respect_request_no_cache_{false};
    bool skip_request_cookie_{false};
    std::string browser_ttl_mode_{"default"};
    std::optional<rule_settings> settings_{};
};

struct policy final {
    std::string id_{};
    std::int64_t revision_{1};
    bool enabled_{true};
    std::string storage_{"tiered"};
    std::int64_t memory_bytes_{64 * 1024 * 1024};
    std::int64_t disk_bytes_{1024 * 1024 * 1024};
    std::int64_t memory_percent_{};
    std::int64_t disk_percent_{};
    std::int64_t memory_reserved_bytes_{};
    std::int64_t disk_reserved_bytes_{};
    std::int64_t max_object_bytes_{512 * 1024 * 1024};
    std::string directory_{"cache"};
    std::vector<rule> rules_{};
};

inline bool configured(const rule& value, std::string_view setting) {
    return value.settings_ && has_setting(*value.settings_, setting);
}
inline std::optional<std::int64_t> status_cache_ttl(const rule& value, std::uint32_t status) {
    if (configured(value, "status")) {
        for (const auto& item : value.settings_->status_ttl_) {
            if (status >= item.from_ && status <= item.to_) {
                return item.seconds_;
            }
        }
    }
    return {};
}
inline bool overrides_origin(const rule& value, std::uint32_t status) {
    return value.ttl_mode_ == "fixed" || status_cache_ttl(value, status).has_value();
}
inline bool origin_control_enabled(const rule& value) {
    return !configured(value, "origin") || value.settings_->origin_control_;
}
// Apply only explicitly configured values. Legacy full-value rules retain their
// existing contract; new rules can inherit settings from earlier matching rules.
inline void apply_rule(rule& target, const rule& source) {
    if (!source.settings_) {
        target = source;
        return;
    }
    target.id_ = source.id_;
    target.name_ = source.name_;
    target.action_ = source.action_;
    target.methods_ = source.methods_;
    target.min_object_bytes_ = source.min_object_bytes_;
    target.max_object_bytes_ = source.max_object_bytes_;
    target.skip_request_cookie_ = source.skip_request_cookie_;
    if (!target.settings_) {
        target.settings_.emplace();
    }
    auto& result = *target.settings_;
    const auto& settings = *source.settings_;
    for (const auto& name : settings.enabled_) {
        if (!has_setting(result, name)) {
            result.enabled_.push_back(name);
        }
        if (name == "edge") {
            target.ttl_mode_ = source.ttl_mode_;
            target.ttl_seconds_ = source.ttl_seconds_;
        } else if (name == "browser") {
            target.browser_ttl_mode_ = source.browser_ttl_mode_;
            target.browser_ttl_seconds_ = source.browser_ttl_seconds_;
        } else if (name == "status") {
            result.status_ttl_ = settings.status_ttl_;
        } else if (name == "key") {
            result.key_ = settings.key_;
            target.ignore_query_ = false;
        } else if (name == "armor") {
            result.armor_ = settings.armor_;
        } else if (name == "device") {
            result.device_ = settings.device_;
        } else if (name == "sort") {
            result.sort_query_ = settings.sort_query_;
        } else if (name == "stale") {
            result.serve_stale_ = settings.serve_stale_;
        } else if (name == "etag") {
            result.strong_etags_ = settings.strong_etags_;
        } else if (name == "origin") {
            result.origin_control_ = settings.origin_control_;
        } else if (name == "vary") {
            result.vary_default_ = settings.vary_default_;
            result.vary_headers_ = settings.vary_headers_;
        } else if (name == "range") {
            result.origin_range_ = settings.origin_range_;
        } else if (name == "ports") {
            result.ports_ = settings.ports_;
        } else if (name == "timeout") {
            result.read_timeout_ = settings.read_timeout_;
        } else if (name == "errors") {
            result.error_passthrough_ = settings.error_passthrough_;
        }
    }
}

[[nodiscard]] inline bool safe_directory(std::string_view value) {
    if (value.empty() || value.size() > 128 || value.front() == '/' || value.front() == '\\' ||
        value.find_first_of(":\\") != std::string_view::npos) {
        return false;
    }
    while (!value.empty()) {
        const auto end = value.find('/');
        const auto part = value.substr(0, end);
        if (part.empty() || part == "." || part == ".." ||
            !std::ranges::all_of(part, [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-';
            })) {
            return false;
        }
        if (end == std::string_view::npos) {
            return true;
        }
        value.remove_prefix(end + 1);
        if (value.empty()) {
            return false;
        }
    }
    return false;
}

// The same bounds apply at the management boundary and on the node.
[[nodiscard]] inline std::string validate(const policy& value) {
    constexpr std::int64_t maximum_capacity = std::int64_t{1} << 50;
    if ((value.storage_ != "memory" && value.storage_ != "disk" && value.storage_ != "tiered") ||
        value.memory_bytes_ < 0 || value.memory_bytes_ > maximum_capacity ||
        value.disk_bytes_ < 0 || value.disk_bytes_ > maximum_capacity ||
        value.memory_percent_ < 0 || value.memory_percent_ > 100 ||
        value.disk_percent_ < 0 || value.disk_percent_ > 100 ||
        value.memory_reserved_bytes_ < 0 || value.memory_reserved_bytes_ > maximum_capacity ||
        value.disk_reserved_bytes_ < 0 || value.disk_reserved_bytes_ > maximum_capacity ||
        value.max_object_bytes_ < 1 || value.max_object_bytes_ > maximum_object_bytes ||
        (value.storage_ == "memory" && value.memory_percent_ == 0 && value.memory_bytes_ < value.max_object_bytes_) ||
        (value.storage_ != "memory" && value.disk_percent_ == 0 && value.disk_bytes_ < value.max_object_bytes_) ||
        (value.storage_ == "tiered" && value.memory_percent_ == 0 && value.memory_bytes_ < 1) ||
        !safe_directory(value.directory_)) {
        return "缓存存储配置不正确，单对象上限为512MiB且不能超过所选存储容量";
    }
    if (value.rules_.size() > 128) {
        return "每个策略最多128条规则";
    }
    std::unordered_set<std::string> ids;
    for (const auto& item : value.rules_) {
        if (item.settings_) {
            if (const auto error = validate_settings(*item.settings_); !error.empty()) {
                return error;
            }
        }
        if (item.id_.empty() || item.id_.size() > 64 || !ids.insert(item.id_).second ||
            item.name_.empty() || !route_policy::clean_text(item.name_, 100) ||
            (item.action_ != "cache" && item.action_ != "bypass") ||
            (item.ttl_mode_ != "origin" && item.ttl_mode_ != "fixed" && item.ttl_mode_ != "origin_strict") ||
            (item.browser_ttl_mode_ != "default" && item.browser_ttl_mode_ != "respect_origin" &&
                item.browser_ttl_mode_ != "override_origin" && item.browser_ttl_mode_ != "bypass") ||
            item.ttl_seconds_ < 0 || item.ttl_seconds_ > 31536000 ||
            item.browser_ttl_seconds_ < 0 || item.browser_ttl_seconds_ > 31536000 ||
            item.min_object_bytes_ < 0 || item.max_object_bytes_ < 0 ||
            item.min_object_bytes_ > value.max_object_bytes_ ||
            item.max_object_bytes_ > value.max_object_bytes_ ||
            (item.max_object_bytes_ != 0 && item.min_object_bytes_ > item.max_object_bytes_)) {
            return "缓存规则名称、标识、时长或内容大小不正确";
        }
        if (item.match_type_ != "all" && item.match_type_ != "exact" && item.match_type_ != "prefix" &&
            item.match_type_ != "extension" && item.match_type_ != "wildcard" && item.match_type_ != "regex" && item.match_type_ != "expression") {
            return "缓存匹配方式不正确";
        }
        if (item.patterns_.size() > 128 || (item.match_type_ == "all" ? !item.patterns_.empty() : item.patterns_.empty())) {
            return "请填写匹配内容，全站匹配无需填写";
        }
        for (const auto& pattern : item.patterns_) {
            if (item.match_type_ == "expression") {
                if (item.patterns_.size() != 1) {
                    return "每条缓存规则只接受一个组合表达式";
                }
                if (const auto error = flexedge::rules::validate_expression(pattern); !error.empty()) {
                    return error;
                }
                continue;
            }
            if (pattern.empty() || !route_policy::clean_text(pattern, 512) ||
                ((item.match_type_ == "exact" || item.match_type_ == "prefix" || item.match_type_ == "wildcard") &&
                    pattern.front() != '/') ||
                (item.match_type_ == "extension" && (pattern.front() != '.' || pattern.size() < 2 ||
                                                        pattern.find_first_of("/?#* ") != std::string::npos)) ||
                (item.match_type_ == "regex" && !route_policy::compile_pattern(pattern))) {
                return "缓存匹配内容不正确";
            }
        }
        if (item.methods_.empty() || item.methods_.size() > 2 ||
            !std::ranges::all_of(item.methods_, [](const auto& method) { return method == "GET" || method == "HEAD"; }) ||
            item.status_codes_.size() > 32 ||
            !std::ranges::all_of(item.status_codes_, [](auto code) { return code >= 200 && code <= 599 && code != 206 && code != 304; })) {
            return "缓存仅支持GET/HEAD及完整响应，请检查方法和状态码";
        }
    }
    return {};
}

template <typename proto_type>
void to_proto(const policy& input, proto_type& output) {
    output.set_id(input.id_);
    output.set_revision(input.revision_);
    output.set_enabled(input.enabled_);
    output.set_storage(input.storage_);
    output.set_memory_bytes(input.memory_bytes_);
    output.set_disk_bytes(input.disk_bytes_);
    output.set_memory_percent(input.memory_percent_);
    output.set_disk_percent(input.disk_percent_);
    output.set_memory_reserved_bytes(input.memory_reserved_bytes_);
    output.set_disk_reserved_bytes(input.disk_reserved_bytes_);
    output.set_max_object_bytes(input.max_object_bytes_);
    output.set_directory(input.directory_);
    for (const auto& source : input.rules_) {
        auto& target = *output.add_rules();
        target.set_id(source.id_);
        target.set_name(source.name_);
        target.set_enabled(source.enabled_);
        target.set_match_type(source.match_type_);
        for (const auto& pattern : source.patterns_) {
            target.add_patterns(pattern);
        }
        target.set_action(source.action_);
        target.set_ttl_mode(source.ttl_mode_);
        target.set_ttl_seconds(source.ttl_seconds_);
        target.set_ignore_query(source.ignore_query_);
        for (const auto& method : source.methods_) {
            target.add_methods(method);
        }
        for (const auto code : source.status_codes_) {
            target.add_status_codes(static_cast<std::uint32_t>(code));
        }
        target.set_min_object_bytes(source.min_object_bytes_);
        target.set_max_object_bytes(source.max_object_bytes_);
        target.set_skip_set_cookie(source.skip_set_cookie_);
        target.set_browser_ttl_seconds(source.browser_ttl_seconds_);
        target.set_skip_no_cache(source.skip_no_cache_);
        target.set_respect_request_no_cache(source.respect_request_no_cache_);
        target.set_skip_request_cookie(source.skip_request_cookie_);
        target.set_browser_ttl_mode(source.browser_ttl_mode_);
        if (source.settings_) {
            settings_to_proto(*source.settings_, *target.mutable_settings());
        }
    }
}

template <typename proto_type>
[[nodiscard]] policy from_proto(const proto_type& input) {
    policy result{.id_ = input.id(), .revision_ = input.revision(), .enabled_ = input.enabled(), .storage_ = input.storage(), .memory_bytes_ = input.memory_bytes(), .disk_bytes_ = input.disk_bytes(), .max_object_bytes_ = input.max_object_bytes(), .directory_ = input.directory()};
    result.memory_percent_ = input.memory_percent();
    result.disk_percent_ = input.disk_percent();
    result.memory_reserved_bytes_ = input.memory_reserved_bytes();
    result.disk_reserved_bytes_ = input.disk_reserved_bytes();
    for (const auto& source : input.rules()) {
        result.rules_.push_back({.id_ = source.id(), .name_ = source.name(), .enabled_ = source.enabled(), .match_type_ = source.match_type(), .patterns_ = {source.patterns().begin(), source.patterns().end()}, .action_ = source.action(), .ttl_mode_ = source.ttl_mode(), .ttl_seconds_ = source.ttl_seconds(), .ignore_query_ = source.ignore_query(), .methods_ = {source.methods().begin(), source.methods().end()}, .status_codes_ = {source.status_codes().begin(), source.status_codes().end()}, .min_object_bytes_ = source.min_object_bytes(), .max_object_bytes_ = source.max_object_bytes(), .skip_set_cookie_ = source.skip_set_cookie(), .browser_ttl_seconds_ = source.browser_ttl_seconds(), .skip_no_cache_ = source.has_skip_no_cache() ? source.skip_no_cache() : true, .respect_request_no_cache_ = source.has_respect_request_no_cache() ? source.respect_request_no_cache() : false, .skip_request_cookie_ = source.has_skip_request_cookie() ? source.skip_request_cookie() : false, .browser_ttl_mode_ = source.has_browser_ttl_mode() ? source.browser_ttl_mode() : source.browser_ttl_seconds() > 0 ? "override_origin"
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                : "default"});
        if (source.has_settings()) {
            result.rules_.back().settings_ = rule_settings_from_proto(source.settings());
        }
    }
    return result;
}

}  // namespace flexedge::cache
