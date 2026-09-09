#pragma once
#include <cctype>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {
namespace compression_validation {
inline void validateCompressionLimits(const v2::Website& website) {
    if (website.response_compression_enabled() &&
        (website.response_compression_min_bytes() < 256 ||
         website.response_compression_min_bytes() > 1024 * 1024 ||
         (website.response_compression_max_bytes() != 0 &&
          website.response_compression_max_bytes() <
              website.response_compression_min_bytes()) ||
         website.response_compression_max_bytes() > 64 * 1024 * 1024 ||
         website.response_compression_algorithms().empty())) {
        throw std::runtime_error("invalid response compression policy");
    }
}

inline void validateCompressionAlgorithms(const v2::Website& website) {
    std::unordered_set<std::string> algorithms;
    for (const auto& algorithm : website.response_compression_algorithms()) {
        if ((algorithm != "br" && algorithm != "zstd" && algorithm != "gzip") ||
            !algorithms.emplace(algorithm).second) {
            throw std::runtime_error("invalid response compression algorithm");
        }
    }
}

inline bool validCompressionMimeType(std::string_view value) noexcept {
    const auto validToken = [](std::string_view segment) {
        return !segment.empty() && std::ranges::all_of(segment, [](unsigned char ch) {
            return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '&' ||
                   ch == '^' || ch == '_' || ch == '.' || ch == '+' || ch == '-';
        });
    };
    const auto slash = value.find('/');
    if (slash == std::string_view::npos ||
        value.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = value.substr(0, slash);
    const auto subtype = value.substr(slash + 1);
    return validToken(type) && (subtype == "*" || validToken(subtype));
}

enum class MatchValueKind { MimeType, Extension };

template <typename Values>
inline void validateCompressionMatchValues(const Values& values, MatchValueKind kind) {
    if (values.size() > 32) {
        throw std::runtime_error("too many response compression match values");
    }
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if (value.empty() || value.size() > 127 ||
            std::ranges::any_of(value,
                                [](unsigned char ch) { return std::isspace(ch) != 0; }) ||
            (kind == MatchValueKind::Extension && value.front() != '.') ||
            (kind == MatchValueKind::MimeType && !validCompressionMimeType(value)) ||
            !seen.emplace(value).second) {
            throw std::runtime_error("invalid response compression match value");
        }
    }
}

} // namespace compression_validation

inline void validateResponseCompression(const v2::Website& website) {
    compression_validation::validateCompressionLimits(website);
    compression_validation::validateCompressionAlgorithms(website);
    compression_validation::validateCompressionMatchValues(
        website.response_compression_mime_types(), compression_validation::MatchValueKind::MimeType);
    compression_validation::validateCompressionMatchValues(
        website.response_compression_extensions(), compression_validation::MatchValueKind::Extension);
    compression_validation::validateCompressionMatchValues(
        website.response_compression_excluded_extensions(), compression_validation::MatchValueKind::Extension);
}

} // namespace flexedge::node
