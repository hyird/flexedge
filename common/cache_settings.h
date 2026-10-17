#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "common/route_match.h"

namespace flexedge::cache {
struct status_ttl final {
    std::int64_t from_{100};
    std::int64_t to_{599};
    std::int64_t seconds_{0};
};
struct header_key final {
    std::string name_{};
    std::vector<std::string> values_{};
};
struct cache_key final {
    std::string query_mode_{"all"};
    std::vector<std::string> query_parameters_{};
    std::vector<std::string> headers_{};
    std::vector<std::string> header_presence_{};
    std::vector<header_key> header_values_{};
    std::vector<std::string> cookies_{};
    std::vector<std::string> cookie_presence_{};
    bool include_origin_{true};
    std::string host_{"original"};
    bool country_{false};
    bool language_{false};
};
struct vary_header final {
    std::string name_{};
    std::string action_{"passthrough"};
    std::vector<std::string> values_{};
};
struct rule_settings final {
    std::vector<std::string> enabled_{};
    std::vector<status_ttl> status_ttl_{};
    cache_key key_{};
    bool armor_{false};
    bool device_{false};
    bool sort_query_{false};
    bool serve_stale_{true};
    bool strong_etags_{false};
    bool origin_control_{true};
    std::string vary_default_{"bypass"};
    std::vector<vary_header> vary_headers_{};
    std::string origin_range_{"default"};
    std::vector<std::int64_t> ports_{};
    std::int64_t read_timeout_{120};
    bool error_passthrough_{false};
};
inline bool has_setting(const rule_settings& settings, std::string_view name) {
    return std::ranges::find(settings.enabled_, name) != settings.enabled_.end();
}
inline std::string validate_settings(const rule_settings& value) {
    constexpr std::array<std::string_view, 15> names{"edge", "status", "browser", "key", "armor", "device", "sort", "stale", "etag", "origin", "vary", "range", "ports", "timeout", "errors"};
    std::unordered_set<std::string> enabled;
    for (const auto& name : value.enabled_) {
        if (std::ranges::find(names, name) == names.end() || !enabled.insert(name).second) {
            return "缓存配置项不正确或重复";
        }
    }
    if (value.status_ttl_.size() > 32 || value.read_timeout_ < 1 || value.read_timeout_ > 6000 ||
        (value.origin_range_ != "default" && value.origin_range_ != "on" && value.origin_range_ != "off") ||
        (value.vary_default_ != "bypass" && value.vary_default_ != "passthrough" && value.vary_default_ != "normalize") || value.vary_headers_.size() > 50 || value.ports_.size() > 64) {
        return "缓存高级设置不正确";
    }
    for (std::size_t i = 0; i < value.status_ttl_.size(); ++i) {
        const auto& status = value.status_ttl_[i];
        if (status.from_ < 100 || status.to_ > 999 || status.from_ > status.to_ || status.seconds_ < -1 || status.seconds_ > 31536000) {
            return "状态码缓存范围或时间不正确";
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (status.from_ <= value.status_ttl_[j].to_ && status.to_ >= value.status_ttl_[j].from_) {
                return "状态码缓存范围不能重叠";
            }
        }
    }
    for (const auto port : value.ports_) {
        if (port < 1 || port > 65535) {
            return "缓存端口必须在1至65535之间";
        }
    }
    const auto& key = value.key_;
    if ((key.query_mode_ != "all" && key.query_mode_ != "ignore" && key.query_mode_ != "include" && key.query_mode_ != "exclude") ||
        (key.host_ != "original" && key.host_ != "resolved") || key.query_parameters_.size() > 100 ||
        ((key.query_mode_ == "include" || key.query_mode_ == "exclude") && key.query_parameters_.empty())) {
        return "自定义缓存键配置不正确";
    }
    for (const auto* fields : {&key.query_parameters_, &key.headers_, &key.header_presence_, &key.cookies_, &key.cookie_presence_}) {
        if (fields->size() > 100) {
            return "缓存键字段过多";
        }
        std::unordered_set<std::string> seen;
        for (const auto& name : *fields) {
            if (name.empty() || !route_policy::clean_text(name, 128) || !seen.insert(name).second || (fields != &key.query_parameters_ && !route_policy::http_token(name))) {
                return "缓存键字段名称不正确或重复";
            }
        }
    }
    if (key.header_values_.size() > 50) {
        return "缓存键请求头条件过多";
    }
    constexpr std::array<std::string_view, 7> restricted_headers{"accept", "accept-charset", "accept-encoding", "accept-datetime", "accept-language", "referer", "user-agent"};
    for (const auto& name : key.headers_) {
        if (std::ranges::any_of(restricted_headers, [&](const auto restricted) { return route_policy::ascii_equal(name, restricted); })) {
            return "此请求头必须使用1至3个指定值区分缓存";
        }
    }
    std::unordered_set<std::string> value_names;
    for (const auto& field : key.header_values_) {
        if (!route_policy::http_token(field.name_) || !value_names.insert(field.name_).second || field.values_.empty() || field.values_.size() > 3 || !std::ranges::all_of(field.values_, [](const auto& text) { return !text.empty() && route_policy::clean_text(text, 1024); })) {
            return "缓存键请求头值配置不正确";
        }
    }
    std::unordered_set<std::string> vary_names;
    constexpr std::array<std::string_view, 12> forbidden_vary{"connection", "host", "cache-control", "transfer-encoding", "keep-alive", "proxy-authenticate", "proxy-authorization", "te", "trailer", "upgrade", "cdn-cache-control", "surrogate-control"};
    for (const auto& header : value.vary_headers_) {
        if (!route_policy::http_token(header.name_) || header.name_.size() > 128 ||
            !std::ranges::all_of(header.name_, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; }) ||
            header.name_.starts_with("cf-") || header.name_.starts_with("cf_") ||
            std::ranges::find(forbidden_vary, header.name_) != forbidden_vary.end() ||
            !vary_names.insert(header.name_).second ||
            (header.action_ != "normalize" && header.action_ != "passthrough" && header.action_ != "bypass") ||
            header.values_.size() > (header.name_ == "accept" ? 10u : 20u)) {
            return "Vary字段或处理方式不正确";
        }
        for (const auto& item : header.values_) {
            if (item.empty() || item.size() > 128 || !std::ranges::all_of(item, [](unsigned char c) { return c >= 32 && c <= 126; })) {
                return "Vary标准化允许值不正确";
            }
        }
    }
    return {};
}
}  // namespace flexedge::cache
