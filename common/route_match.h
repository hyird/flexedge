#pragma once

#include <re2/re2.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/route_policy.h"

namespace flexedge::route_policy {

inline bool redirect_status(std::int64_t status) {
    return status == 301 || status == 302 || status == 307 || status == 308;
}

inline bool valid_match_type(std::string_view type) {
    return type == "exact" || type == "prefix" || type == "suffix" || type == "regex";
}

inline std::shared_ptr<const RE2> compile_pattern(std::string_view pattern) {
    if (pattern.empty() || pattern.size() > 512 || !clean_text(pattern, 512) ||
        pattern.find("\\C") != std::string_view::npos) {
        return {};
    }
    RE2::Options options;
    options.set_log_errors(false);
    options.set_max_mem(1 << 20);
    auto result = std::make_shared<const RE2>(std::string(pattern), options);
    if (!result->ok() || result->NumberOfCapturingGroups() > 9) {
        return {};
    }
    return result;
}

inline bool capture_template(std::string_view value, int groups) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '$' || i + 1 == value.size()) {
            continue;
        }
        if (value[i + 1] == '$') {
            ++i;
            continue;
        }
        if (value[i + 1] != '{') {
            continue;
        }
        if (i + 3 >= value.size() || value[i + 2] < '0' || value[i + 2] > '9' ||
            value[i + 3] != '}' || value[i + 2] - '0' > groups) {
            return false;
        }
        i += 3;
    }
    return true;
}

inline bool condition_options(std::string_view source, std::string_view name,
    std::string_view op, std::string_view value) {
    if ((source != "header" && source != "query") || name.empty() ||
        !clean_text(name, 256) || !clean_text(value, 2048) ||
        (op != "equals" && op != "not_equals" && op != "exists" && op != "absent") ||
        ((op == "exists" || op == "absent") && !value.empty())) {
        return false;
    }
    if (source == "header" && !http_token(name, 256)) {
        return false;
    }
    return true;
}

inline bool valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) {
            continue;
        }
        unsigned int code = 0, minimum = 0;
        std::size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            code = first & 0x1f;
            remaining = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            code = first & 0x0f;
            remaining = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            code = first & 0x07;
            remaining = 3;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (i + remaining > value.size()) {
            return false;
        }
        while (remaining-- > 0) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
            return false;
        }
    }
    return true;
}

inline std::optional<std::string> decode_query(std::string_view input) {
    std::string result;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') {
            result.push_back(' ');
        } else if (input[i] == '%') {
            if (i + 2 >= input.size() || hex(input[i + 1]) < 0 || hex(input[i + 2]) < 0) {
                return {};
            }
            result.push_back(static_cast<char>(hex(input[i + 1]) * 16 + hex(input[i + 2])));
            i += 2;
        } else {
            result.push_back(input[i]);
        }
    }
    if (!valid_utf8(result)) {
        return {};
    }
    return result;
}

using query_values = std::vector<std::pair<std::string, std::string>>;
using header_values = std::vector<std::pair<std::string_view, std::string_view>>;
inline std::optional<query_values> get_query_values(std::string_view target) {
    query_values result;
    const auto question = target.find('?');
    if (question == std::string_view::npos) {
        return result;
    }
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        if (!part.empty()) {
            const auto equal = part.find('=');
            auto name = decode_query(part.substr(0, equal));
            auto value = decode_query(equal == std::string_view::npos ? std::string_view{} : part.substr(equal + 1));
            if (!name || !value) {
                return {};
            }
            result.emplace_back(std::move(*name), std::move(*value));
        }
        if (amp == std::string_view::npos) {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return result;
}

}  // namespace flexedge::route_policy
