#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/route_match.h"

namespace flexedge::cache {

inline constexpr std::int64_t maximum_object_bytes{256 * 1024 * 1024};

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
    std::vector<std::int64_t> status_codes_{200};
    std::int64_t min_object_bytes_{0};
    std::int64_t max_object_bytes_{0};
    bool skip_set_cookie_{true};
    std::int64_t browser_ttl_seconds_{0};
    bool skip_no_cache_{true};
    bool respect_request_no_cache_{true};
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
    std::int64_t max_object_bytes_{64 * 1024 * 1024};
    std::string directory_{"cache"};
    std::vector<rule> rules_{};
};

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
        return "缓存存储配置不正确，单对象上限为256MiB且不能超过所选存储容量";
    }
    if (value.rules_.size() > 128) {
        return "每个策略最多128条规则";
    }
    std::unordered_set<std::string> ids;
    for (const auto& item : value.rules_) {
        if (item.id_.empty() || item.id_.size() > 64 || !ids.insert(item.id_).second ||
            item.name_.empty() || !route_policy::clean_text(item.name_, 100) ||
            (item.action_ != "cache" && item.action_ != "bypass") ||
            (item.ttl_mode_ != "origin" && item.ttl_mode_ != "fixed") ||
            item.ttl_seconds_ < 1 || item.ttl_seconds_ > 31536000 ||
            item.browser_ttl_seconds_ < 0 || item.browser_ttl_seconds_ > 31536000 ||
            item.min_object_bytes_ < 0 || item.max_object_bytes_ < 0 ||
            item.min_object_bytes_ > value.max_object_bytes_ ||
            item.max_object_bytes_ > value.max_object_bytes_ ||
            (item.max_object_bytes_ != 0 && item.min_object_bytes_ > item.max_object_bytes_)) {
            return "缓存规则名称、标识、时长或内容大小不正确";
        }
        if (item.match_type_ != "all" && item.match_type_ != "exact" && item.match_type_ != "prefix" &&
            item.match_type_ != "extension" && item.match_type_ != "wildcard" && item.match_type_ != "regex") {
            return "缓存匹配方式不正确";
        }
        if (item.patterns_.size() > 64 || (item.match_type_ == "all" ? !item.patterns_.empty() : item.patterns_.empty())) {
            return "请填写匹配内容，全站匹配无需填写";
        }
        for (const auto& pattern : item.patterns_) {
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
            item.status_codes_.empty() || item.status_codes_.size() > 32 ||
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
        result.rules_.push_back({.id_ = source.id(), .name_ = source.name(), .enabled_ = source.enabled(), .match_type_ = source.match_type(), .patterns_ = {source.patterns().begin(), source.patterns().end()}, .action_ = source.action(), .ttl_mode_ = source.ttl_mode(), .ttl_seconds_ = source.ttl_seconds(), .ignore_query_ = source.ignore_query(), .methods_ = {source.methods().begin(), source.methods().end()}, .status_codes_ = {source.status_codes().begin(), source.status_codes().end()}, .min_object_bytes_ = source.min_object_bytes(), .max_object_bytes_ = source.max_object_bytes(), .skip_set_cookie_ = source.skip_set_cookie(), .browser_ttl_seconds_ = source.browser_ttl_seconds(), .skip_no_cache_ = source.has_skip_no_cache() ? source.skip_no_cache() : true, .respect_request_no_cache_ = source.has_respect_request_no_cache() ? source.respect_request_no_cache() : true});
    }
    return result;
}

}  // namespace flexedge::cache
