#pragma once

#include <ruvia/http/http_content_codec.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include "node/data/buffered_proxy_response.h"
#include "node/data/origin_request_codec.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline std::string_view response_header(const buffered_proxy_response& response,
    std::string_view name) noexcept {
    for (auto iterator = response.headers_.rbegin(); iterator != response.headers_.rend();
        ++iterator) {
        if (http_header_name(iterator->first, name)) {
            return iterator->second;
        }
    }
    return {};
}

inline bool ascii_equal(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
               [](unsigned char a, unsigned char b) {
                   return static_cast<unsigned char>(std::tolower(a)) ==
                          static_cast<unsigned char>(std::tolower(b));
               });
}

inline bool ascii_starts_with(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && ascii_equal(value.substr(0, prefix.size()), prefix);
}

inline bool matches_compression_mime(const v2::Website& website, std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    value = value.substr(0, semicolon);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    for (const auto& configured : website.response_compression_mime_types()) {
        const std::string_view pattern(configured);
        if (pattern.ends_with("/*")) {
            if (ascii_starts_with(value, pattern.substr(0, pattern.size() - 1))) {
                return true;
            }
        } else if (ascii_equal(value, pattern)) {
            return true;
        }
    }
    return false;
}

inline std::string_view request_extension(std::string_view target) noexcept {
    target = target.substr(0, target.find_first_of("?#"));
    const auto slash = target.rfind('/');
    const auto dot = target.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return target.substr(dot);
}

inline bool matches_compression_extension(const auto& configured,
    std::string_view extension) noexcept {
    return std::ranges::any_of(configured, [extension](const auto& value) {
        return ascii_equal(extension, std::string_view(value));
    });
}

inline std::optional<ruvia::http_content_coding> configured_coding(std::string_view value) noexcept {
    if (value == "br") {
        return ruvia::http_content_coding::brotli;
    }
    if (value == "zstd") {
        return ruvia::http_content_coding::zstd;
    }
    if (value == "gzip") {
        return ruvia::http_content_coding::gzip;
    }
    return std::nullopt;
}

inline std::string_view trim_encoding_whitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

struct encoding_quality_item final {
    std::string_view token_;
    double quality_{1};
};

inline encoding_quality_item parse_encoding_quality_item(std::string_view item) noexcept {
    item = trim_encoding_whitespace(item);
    const auto semicolon = item.find(';');
    const auto token = trim_encoding_whitespace(item.substr(0, semicolon));
    double quality = 1;
    if (semicolon != std::string_view::npos) {
        const auto parameters = item.substr(semicolon + 1);
        const auto equal = parameters.find('=');
        if (equal != std::string_view::npos) {
            const auto value = trim_encoding_whitespace(parameters.substr(equal + 1));
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), quality);
            if (parsed.ec != std::errc{}) {
                quality = 0;
            }
        }
    }
    return {.token_ = token, .quality_ = quality};
}

inline double encoding_quality(std::string_view field, std::string_view wanted) noexcept {
    double wildcard = -1;
    while (!field.empty()) {
        const auto comma = field.find(',');
        const auto parsed = parse_encoding_quality_item(field.substr(0, comma));
        if (http_header_name(parsed.token_, wanted)) {
            return parsed.quality_;
        }
        if (parsed.token_ == "*") {
            wildcard = parsed.quality_;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        field.remove_prefix(comma + 1);
    }
    return wildcard < 0 ? 0 : wildcard;
}

// `target` is the request target sent to the origin and `accept_encoding` the client's
// Accept-Encoding field value.
inline void apply_response_compression(const v2::Website& website, std::string_view target,
    std::string_view accept_encoding, buffered_proxy_response& response) {
    if (!website.response_compression_enabled() || response.status_ == 206) {
        return;
    }
    const auto extension = request_extension(target);
    if (!extension.empty() && matches_compression_extension(
                                  website.response_compression_excluded_extensions(), extension)) {
        return;
    }
    if (!matches_compression_mime(website, response_header(response, "Content-Type")) &&
        (extension.empty() ||
            !matches_compression_extension(website.response_compression_extensions(), extension))) {
        return;
    }
    if (accept_encoding.empty()) {
        return;
    }
    auto coding = ruvia::http_content_coding::identity;
    double quality = 0;
    for (const auto& configured : website.response_compression_algorithms()) {
        const auto candidate = configured_coding(configured);
        if (!candidate) {
            continue;
        }
        const auto candidate_quality =
            encoding_quality(accept_encoding, ruvia::http_content_coding_token(*candidate));
        if (candidate_quality > quality) {
            coding = *candidate;
            quality = candidate_quality;
        }
    }
    if (coding == ruvia::http_content_coding::identity || quality <= 0) {
        return;
    }

    const auto original_size = response.body_.size();
    std::string_view plain_body = response.body_;
    const auto content_encoding = response_header(response, "Content-Encoding");
    if (!content_encoding.empty() && !ascii_equal(content_encoding, "identity")) {
        return;
    }
    if (plain_body.size() < website.response_compression_min_bytes() ||
        (website.response_compression_max_bytes() != 0 &&
            plain_body.size() > website.response_compression_max_bytes()) ||
        original_size == 0) {
        return;
    }

    // The encoder owns a second copy while the original response remains available. Under load,
    // keep serving the origin representation rather than letting compression bypass admission.
    if (!response.reservation_.try_grow(original_size - 1)) {
        return;
    }
    {
        auto encoded =
            ruvia::encode_http_content(coding, plain_body, {.max_encoded_bytes_ = original_size - 1});
        if (encoded.encoded() == nullptr) {
            response.reservation_.shrink(original_size - 1);
            return;
        }
        response.body_.assign(encoded.encoded()->bytes());
    }
    response.reservation_.shrink(original_size - 1);
    response.headers_.erase(std::remove_if(response.headers_.begin(), response.headers_.end(),
                                [](const auto& header) {
                                    return http_header_name(header.first, "Content-Encoding");
                                }),
        response.headers_.end());
    response.headers_.emplace_back("Content-Encoding", ruvia::http_content_coding_token(coding));
    const auto vary = std::string(response_header(response, "Vary"));
    if (vary.empty()) {
        response.headers_.emplace_back("Vary", "Accept-Encoding");
    } else if (vary.find("Accept-Encoding") == std::string::npos) {
        response.headers_.erase(
            std::remove_if(response.headers_.begin(), response.headers_.end(),
                [](const auto& header) { return http_header_name(header.first, "Vary"); }),
            response.headers_.end());
        response.headers_.emplace_back("Vary", vary + ", Accept-Encoding");
    }
}

}  // namespace flexedge::node
