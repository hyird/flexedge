#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/route_match.h"

namespace flexedge::node {

inline constexpr std::size_t origin_range_block_bytes = 1024 * 1024;

struct cache_byte_range final {
    std::size_t offset_{}, length_{};
    bool satisfiable_{};
};

inline std::optional<std::size_t> range_number(std::string_view value) {
    std::size_t number{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return number;
}

inline std::optional<cache_byte_range> parse_cache_range(std::string_view field, std::size_t size) {
    if (!field.starts_with("bytes=") || field.find(',') != std::string_view::npos) {
        return std::nullopt;
    }
    field.remove_prefix(6);
    if (field.empty()) {
        return cache_byte_range{};
    }
    const auto dash = field.find('-');
    if (dash == std::string_view::npos) {
        return std::nullopt;
    }
    if (dash == 0) {
        const auto suffix = range_number(field.substr(1));
        if (!suffix) {
            return std::nullopt;
        }
        const auto length = (std::min)(*suffix, size);
        return cache_byte_range{size - length, length, length != 0};
    }
    const auto start = range_number(field.substr(0, dash));
    const auto tail = field.substr(dash + 1);
    const auto end = tail.empty() ? std::optional<std::size_t>{size ? size - 1 : 0} : range_number(tail);
    if (!start || !end) {
        return std::nullopt;
    }
    if (*start >= size || *end < *start) {
        return cache_byte_range{};
    }
    return cache_byte_range{*start, (std::min)(*end, size - 1) - *start + 1, true};
}

struct cache_range_selection final {
    std::vector<cache_byte_range> ranges_{};
    bool ignore_{false};
    bool multipart_{false};
};

inline cache_range_selection select_cache_ranges(std::string_view value, std::size_t size, bool partial_file) {
    cache_range_selection result;
    if (!value.starts_with("bytes=")) {
        result.ignore_ = true;
        return result;
    }
    value.remove_prefix(6);
    std::size_t count{};
    std::size_t previous_end{};
    bool has_previous = false;
    do {
        if (++count > 300 || (!partial_file && count == 300)) {
            result.ignore_ = true;
            return result;
        }
        const auto comma = value.find(',');
        const auto item = flexedge::route_policy::trim_ows(value.substr(0, comma));
        const auto range = parse_cache_range(std::string("bytes=") + std::string(item), size);
        if (!range) {
            result.ignore_ = true;
            return result;
        }
        if (range->satisfiable_) {
            if (has_previous && range->offset_ < previous_end) {
                result.ranges_.clear();
                result.ignore_ = !partial_file;
                return result;
            }
            previous_end = range->offset_ + range->length_;
            has_previous = true;
            result.ranges_.push_back(*range);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        value.remove_prefix(comma + 1);
    } while (true);
    result.multipart_ = result.ranges_.size() > 1 || (partial_file && count > 1 && !result.ranges_.empty());
    return result;
}

struct origin_content_range final {
    std::size_t first_{}, last_{}, total_{};
};

inline std::optional<origin_content_range> parse_origin_content_range(std::string_view value) {
    if (!value.starts_with("bytes ")) {
        return std::nullopt;
    }
    value.remove_prefix(6);
    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == std::string_view::npos || slash == std::string_view::npos || dash >= slash) {
        return std::nullopt;
    }
    const auto first = range_number(value.substr(0, dash));
    const auto last = range_number(value.substr(dash + 1, slash - dash - 1));
    const auto total = range_number(value.substr(slash + 1));
    if (!first || !last || !total || *first > *last || *last >= *total) {
        return std::nullopt;
    }
    return origin_content_range{*first, *last, *total};
}

}  // namespace flexedge::node
