#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <re2/re2.h>
#include "common/route_policy.h"

namespace flexedge::route_policy {

inline bool redirectStatus(std::int64_t status) {
    return status == 301 || status == 302 || status == 307 || status == 308;
}

inline std::string_view redirectStatusLine(std::int64_t status) {
    switch (status) {
    case 301: return "301 Moved Permanently";
    case 307: return "307 Temporary Redirect";
    case 308: return "308 Permanent Redirect";
    default: return "302 Found";
    }
}

inline int matchPriority(std::string_view type) {
    if (type == "exact") return 4;
    if (type == "prefix") return 3;
    if (type == "suffix") return 2;
    if (type == "regex") return 1;
    return 0;
}

inline std::shared_ptr<const RE2> compilePattern(std::string_view pattern) {
    if (pattern.empty() || pattern.size() > 512 || !cleanText(pattern, 512) ||
        pattern.find("\\C") != std::string_view::npos) return {};
    RE2::Options options;
    options.set_log_errors(false);
    options.set_max_mem(1 << 20);
    auto result = std::make_shared<const RE2>(std::string(pattern), options);
    if (!result->ok() || result->NumberOfCapturingGroups() > 9) return {};
    return result;
}

inline bool captureTemplate(std::string_view value, int groups) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '$' || i + 1 == value.size()) continue;
        if (value[i + 1] == '$') { ++i; continue; }
        if (value[i + 1] != '{') continue;
        if (i + 3 >= value.size() || value[i + 2] < '0' || value[i + 2] > '9' ||
            value[i + 3] != '}' || value[i + 2] - '0' > groups) return false;
        i += 3;
    }
    return true;
}

inline std::optional<std::string> expandCaptures(std::string_view value,
                                                std::string_view path, const RE2& pattern) {
    std::array<absl::string_view, 10> captures{};
    if (!pattern.Match(absl::string_view(path.data(), path.size()), 0, path.size(),
                       RE2::UNANCHORED, captures.data(), pattern.NumberOfCapturingGroups() + 1)) return {};
    std::string result;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '$' && i + 1 < value.size() && value[i + 1] == '$') {
            result.push_back('$'); ++i;
        } else if (value[i] == '$' && i + 3 < value.size() && value[i + 1] == '{' &&
                   value[i + 2] >= '0' && value[i + 2] <= '9' && value[i + 3] == '}') {
            const auto& capture = captures[static_cast<std::size_t>(value[i + 2] - '0')];
            if (!capture.empty()) result.append(capture.data(), capture.size());
            i += 3;
        } else result.push_back(value[i]);
        if (result.size() > 16384) return {};
    }
    // Captures never turn an encoded request path into response header injection.
    if (!cleanText(result, 16384) || result.find(' ') != std::string::npos) return {};
    return result;
}

inline bool conditionOptions(std::string_view source, std::string_view name,
                             std::string_view op, std::string_view value) {
    if ((source != "header" && source != "query") || name.empty() ||
        !cleanText(name, 256) || !cleanText(value, 2048) ||
        (op != "equals" && op != "not_equals" && op != "exists" && op != "absent") ||
        ((op == "exists" || op == "absent") && !value.empty())) return false;
    if (source == "header") {
        for (const auto c : name) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return false;
        }
    }
    return true;
}

inline bool asciiEqual(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
        if (lower(left[i]) != lower(right[i])) return false;
    }
    return true;
}

inline std::string_view trimOws(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    return value;
}

inline bool validUtf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned int code = 0, minimum = 0;
        std::size_t remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) { code = first & 0x1f; remaining = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { code = first & 0x0f; remaining = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { code = first & 0x07; remaining = 3; minimum = 0x10000; }
        else return false;
        if (i + remaining > value.size()) return false;
        while (remaining-- > 0) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

inline std::optional<std::string> decodeQuery(std::string_view input) {
    std::string result;
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') result.push_back(' ');
        else if (input[i] == '%') {
            if (i + 2 >= input.size() || hex(input[i + 1]) < 0 || hex(input[i + 2]) < 0) return {};
            result.push_back(static_cast<char>(hex(input[i + 1]) * 16 + hex(input[i + 2]))); i += 2;
        } else result.push_back(input[i]);
    }
    if (!validUtf8(result)) return {};
    return result;
}

using QueryValues = std::vector<std::pair<std::string, std::string>>;
using HeaderValues = std::vector<std::pair<std::string_view, std::string_view>>;
inline std::optional<QueryValues> queryValues(std::string_view target) {
    QueryValues result;
    const auto question = target.find('?');
    if (question == std::string_view::npos) return result;
    auto query = target.substr(question + 1);
    while (!query.empty()) {
        const auto amp = query.find('&');
        const auto part = query.substr(0, amp);
        if (!part.empty()) {
            const auto equal = part.find('=');
            auto name = decodeQuery(part.substr(0, equal));
            auto value = decodeQuery(equal == std::string_view::npos ? std::string_view{} : part.substr(equal + 1));
            if (!name || !value) return {};
            result.emplace_back(std::move(*name), std::move(*value));
        }
        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return result;
}

inline bool conditionMatches(std::string_view source, std::string_view name,
                             std::string_view op, std::string_view value,
                             std::span<const std::pair<std::string_view, std::string_view>> headers,
                             const std::optional<QueryValues>& query) {
    bool present = false, equal = false;
    if (source == "header") {
        for (const auto& [key, item] : headers) if (asciiEqual(key, name)) {
            present = true; equal = equal || trimOws(item) == value;
        }
    } else {
        if (!query) return false;
        for (const auto& [key, item] : *query) if (key == name) {
            present = true; equal = equal || item == value;
        }
    }
    if (op == "exists") return present;
    if (op == "absent") return !present;
    if (op == "not_equals") return present && !equal;
    return op == "equals" && equal;
}

} // namespace flexedge::route_policy
