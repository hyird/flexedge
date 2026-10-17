#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/cache_policy.h"

namespace flexedge::node {
// The result borrows target unless query rewriting needs caller-owned storage.
inline std::string_view cache_query_target(std::string_view target, const flexedge::cache::rule& rule, std::string& rewritten) {
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return target;
    }
    const auto* key = flexedge::cache::configured(rule, "key") ? &rule.settings_->key_ : nullptr;
    if (rule.ignore_query_ || (key && key->query_mode_ == "ignore")) {
        return target.substr(0, question);
    }
    const auto sort = flexedge::cache::configured(rule, "sort") && rule.settings_->sort_query_;
    if (!sort && (!key || key->query_mode_ == "all")) {
        return target;
    }
    struct parameter final {
        std::string_view raw_;
        std::string name_, value_;
    };
    std::vector<parameter> parameters;
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto ampersand = query.find('&');
        const auto raw = query.substr(0, ampersand);
        const auto equal = raw.find('=');
        const auto name = flexedge::route_policy::decode_query(raw.substr(0, equal));
        const auto value = flexedge::route_policy::decode_query(equal == std::string_view::npos ? std::string_view{} : raw.substr(equal + 1));
        if (!name || !value) {
            return target;
        }
        const auto listed = key && std::ranges::find(key->query_parameters_, *name) != key->query_parameters_.end();
        if (!key || key->query_mode_ == "all" || (key->query_mode_ == "include" && listed) || (key->query_mode_ == "exclude" && !listed)) {
            parameters.push_back({raw, *name, *value});
        }
        query = ampersand == std::string_view::npos ? std::string_view{} : query.substr(ampersand + 1);
    }
    if (sort) {
        std::ranges::stable_sort(parameters, [](const auto& left, const auto& right) { return left.name_ != right.name_ ? left.name_ < right.name_ : left.value_ < right.value_; });
    }
    rewritten.assign(target.substr(0, question));
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        rewritten += index == 0 ? '?' : '&';
        rewritten += parameters[index].raw_;
    }
    return rewritten;
}
inline void append_cache_key_part(std::string& key, std::string_view part) {
    key.append(std::to_string(part.size())).append(":").append(part);
}
inline std::string_view cache_device_type(std::string_view agent) {
    std::string lower(agent);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return flexedge::route_policy::lower_ascii(c); });
    if (lower.find("ipad") != std::string::npos || lower.find("tablet") != std::string::npos || (lower.find("android") != std::string::npos && lower.find("mobile") == std::string::npos)) {
        return "tablet";
    }
    if (lower.find("mobile") != std::string::npos || lower.find("iphone") != std::string::npos || lower.find("ipod") != std::string::npos || lower.find("windows phone") != std::string::npos) {
        return "mobile";
    }
    return "desktop";
}
}  // namespace flexedge::node
