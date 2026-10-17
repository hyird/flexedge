#pragma once
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "common/cache_policy.h"

namespace flexedge::node {
inline const flexedge::cache::vary_header* cache_vary_header(const flexedge::cache::rule& rule, std::string_view name) {
    if (!flexedge::cache::configured(rule, "vary")) {
        return nullptr;
    }
    for (const auto& field : rule.settings_->vary_headers_) {
        if (flexedge::route_policy::ascii_equal(field.name_, name)) {
            return &field;
        }
    }
    return nullptr;
}
inline std::string_view cache_vary_action(const flexedge::cache::rule& rule, std::string_view name) {
    if (const auto* field = cache_vary_header(rule, name)) {
        return field->action_;
    }
    if (!flexedge::cache::configured(rule, "vary")) {
        return flexedge::route_policy::ascii_equal(name, "accept-encoding") ? "normalize" : "ignore";
    }
    const auto* field = cache_vary_header(rule, name);
    return field ? std::string_view(field->action_) : std::string_view(rule.settings_->vary_default_);
}
inline std::string normalize_cache_vary(std::string_view input, std::string_view name, const std::vector<std::string>& allowed = {}) {
    if (name != "accept" && name != "accept-language" && name != "accept-encoding") {
        return std::string(flexedge::route_policy::trim_ows(input));
    }
    struct token final {
        std::string name_;
        unsigned quality_{1000};
    };
    std::vector<token> tokens;
    while (!input.empty()) {
        const auto comma = input.find(',');
        auto part = flexedge::route_policy::trim_ows(input.substr(0, comma));
        input = comma == std::string_view::npos ? std::string_view{} : input.substr(comma + 1);
        const auto semicolon = part.find(';');
        std::string value(flexedge::route_policy::trim_ows(part.substr(0, semicolon)));
        std::ranges::transform(value, value.begin(), [](unsigned char c) { return flexedge::route_policy::lower_ascii(c); });
        if (value.empty()) {
            continue;
        }
        unsigned quality = 1000;
        if (semicolon != std::string_view::npos) {
            auto parameters = part.substr(semicolon + 1);
            while (!parameters.empty()) {
                const auto next = parameters.find(';');
                const auto parameter = flexedge::route_policy::trim_ows(parameters.substr(0, next));
                parameters = next == std::string_view::npos ? std::string_view{} : parameters.substr(next + 1);
                if (parameter.starts_with("q=")) {
                    const auto text = parameter.substr(2);
                    if (text.empty() || (text[0] != '0' && text[0] != '1')) {
                        quality = 0;
                        continue;
                    }
                    quality = text[0] == '1' ? 1000 : 0;
                    if (text.size() > 1) {
                        if (text[1] != '.' || text.size() > 5) {
                            quality = 0;
                            continue;
                        }
                        unsigned scale = 100;
                        for (std::size_t index = 2; index < text.size(); ++index) {
                            if (text[index] < '0' || text[index] > '9' || (text[0] == '1' && text[index] != '0')) {
                                quality = 0;
                                break;
                            }
                            quality += (text[index] - '0') * scale;
                            scale /= 10;
                        }
                    }
                }
            }
        }
        if (name == "accept-language" && (allowed.empty() || std::ranges::find(allowed, value) == allowed.end())) {
            value.resize(value.find('-') == std::string::npos ? value.size() : value.find('-'));
        }
        if (!allowed.empty() && std::ranges::find(allowed, value) == allowed.end()) {
            continue;
        }
        auto found = std::ranges::find(tokens, value, &token::name_);
        if (found == tokens.end()) {
            tokens.push_back({std::move(value), quality});
        } else {
            found->quality_ = (std::max)(found->quality_, quality);
        }
    }
    std::ranges::sort(tokens, [](const auto& left, const auto& right) { return left.quality_ != right.quality_ ? left.quality_ > right.quality_ : left.name_ < right.name_; });
    std::string result;
    for (const auto& token : tokens) {
        if (!result.empty()) {
            result += ',';
        }
        result += token.name_;
        if (token.quality_ == 0) {
            result += ";q=0";
        }
    }
    return result;
}
inline std::string cache_vary_value(const flexedge::cache::rule& rule, std::string_view name, std::string raw, std::string_view encoding) {
    if (name == "accept-encoding" && cache_vary_action(rule, name) == "normalize" && !(flexedge::cache::configured(rule, "etag") && rule.settings_->strong_etags_)) {
        return std::string(encoding);
    }
    const auto* field = cache_vary_header(rule, name);
    return cache_vary_action(rule, name) == "normalize" ? normalize_cache_vary(raw, name, field ? field->values_ : std::vector<std::string>{}) : raw;
}
}  // namespace flexedge::node
