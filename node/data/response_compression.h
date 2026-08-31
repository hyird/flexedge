#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <ruvia/http/HttpContentCodec.h>

#include "node/data/buffered_origin_exchange.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline std::string_view proxyHeader(const BufferedProxyRequest& request,
                                    std::string_view name) noexcept {
    for (auto iterator = request.headers.rbegin(); iterator != request.headers.rend(); ++iterator) {
        if (httpHeaderName(iterator->first, name)) {
            return iterator->second;
        }
    }
    return {};
}

inline std::string_view responseHeader(const BufferedProxyResponse& response,
                                       std::string_view name) noexcept {
    for (auto iterator = response.headers.rbegin(); iterator != response.headers.rend();
         ++iterator) {
        if (httpHeaderName(iterator->first, name)) {
            return iterator->second;
        }
    }
    return {};
}

inline bool asciiEqual(std::string_view left, std::string_view right) noexcept {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(),
                      [](unsigned char a, unsigned char b) {
                          return static_cast<unsigned char>(std::tolower(a)) ==
                                 static_cast<unsigned char>(std::tolower(b));
                      });
}

inline bool asciiStartsWith(std::string_view value, std::string_view prefix) noexcept {
    return value.size() >= prefix.size() && asciiEqual(value.substr(0, prefix.size()), prefix);
}

inline bool matchesCompressionMime(const v2::Website& website, std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    value = value.substr(0, semicolon);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    for (const auto& configured : website.response_compression_mime_types()) {
        const std::string_view pattern(configured);
        if (pattern.ends_with("/*")) {
            if (asciiStartsWith(value, pattern.substr(0, pattern.size() - 1))) {
                return true;
            }
        } else if (asciiEqual(value, pattern)) {
            return true;
        }
    }
    return false;
}

inline std::string_view requestExtension(std::string_view target) noexcept {
    target = target.substr(0, target.find_first_of("?#"));
    const auto slash = target.rfind('/');
    const auto dot = target.rfind('.');
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return target.substr(dot);
}

inline bool matchesCompressionExtension(const auto& configured,
                                        std::string_view extension) noexcept {
    return std::ranges::any_of(configured, [extension](const auto& value) {
        return asciiEqual(extension, std::string_view(value));
    });
}

inline std::optional<ruvia::HttpContentCoding> configuredCoding(std::string_view value) noexcept {
    if (value == "br") {
        return ruvia::HttpContentCoding::kBrotli;
    }
    if (value == "zstd") {
        return ruvia::HttpContentCoding::kZstd;
    }
    if (value == "gzip") {
        return ruvia::HttpContentCoding::kGzip;
    }
    return std::nullopt;
}

inline std::string_view trimEncodingWhitespace(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

struct EncodingQualityItem final {
    std::string_view token;
    double quality{1};
};

inline EncodingQualityItem parseEncodingQualityItem(std::string_view item) noexcept {
    item = trimEncodingWhitespace(item);
    const auto semicolon = item.find(';');
    const auto token = trimEncodingWhitespace(item.substr(0, semicolon));
    double quality = 1;
    if (semicolon != std::string_view::npos) {
        const auto parameters = item.substr(semicolon + 1);
        const auto equal = parameters.find('=');
        if (equal != std::string_view::npos) {
            const auto value = trimEncodingWhitespace(parameters.substr(equal + 1));
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), quality);
            if (parsed.ec != std::errc{}) {
                quality = 0;
            }
        }
    }
    return {.token = token, .quality = quality};
}

inline double encodingQuality(std::string_view field, std::string_view wanted) noexcept {
    double wildcard = -1;
    while (!field.empty()) {
        const auto comma = field.find(',');
        const auto parsed = parseEncodingQualityItem(field.substr(0, comma));
        if (httpHeaderName(parsed.token, wanted)) {
            return parsed.quality;
        }
        if (parsed.token == "*") {
            wildcard = parsed.quality;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        field.remove_prefix(comma + 1);
    }
    return wildcard < 0 ? 0 : wildcard;
}

inline void applyResponseCompression(const BufferedProxyRequest& request,
                                     const v2::Website& website, BufferedProxyResponse& response) {
    if (!website.response_compression_enabled() || response.status == 206) {
        return;
    }
    const auto extension = requestExtension(request.target);
    if (!extension.empty() && matchesCompressionExtension(
                                  website.response_compression_excluded_extensions(), extension)) {
        return;
    }
    if (!matchesCompressionMime(website, responseHeader(response, "Content-Type")) &&
        (extension.empty() ||
         !matchesCompressionExtension(website.response_compression_extensions(), extension))) {
        return;
    }
    const auto accepted = proxyHeader(request, "Accept-Encoding");
    if (accepted.empty()) {
        return;
    }
    auto coding = ruvia::HttpContentCoding::kIdentity;
    double quality = 0;
    for (const auto& configured : website.response_compression_algorithms()) {
        const auto candidate = configuredCoding(configured);
        if (!candidate) {
            continue;
        }
        const auto candidateQuality =
            encodingQuality(accepted, ruvia::httpContentCodingToken(*candidate));
        if (candidateQuality > quality) {
            coding = *candidate;
            quality = candidateQuality;
        }
    }
    if (coding == ruvia::HttpContentCoding::kIdentity || quality <= 0) {
        return;
    }

    const auto originalSize = response.body.size();
    std::string_view plainBody = response.body;
    const auto contentEncoding = responseHeader(response, "Content-Encoding");
    if (!contentEncoding.empty() && !asciiEqual(contentEncoding, "identity")) {
        return;
    }
    if (plainBody.size() < website.response_compression_min_bytes() ||
        (website.response_compression_max_bytes() != 0 &&
         plainBody.size() > website.response_compression_max_bytes()) ||
        originalSize == 0) {
        return;
    }

    // The encoder owns a second copy while the original response remains available. Under load,
    // keep serving the origin representation rather than letting compression bypass admission.
    if (!response.reservation.tryGrow(originalSize - 1)) {
        return;
    }
    {
        auto encoded =
            ruvia::encodeHttpContent(coding, plainBody, {.maxEncodedBytes = originalSize - 1});
        if (encoded.encoded() == nullptr) {
            response.reservation.shrink(originalSize - 1);
            return;
        }
        response.body.assign(encoded.encoded()->bytes());
    }
    response.reservation.shrink(originalSize - 1);
    response.headers.erase(
        std::remove_if(response.headers.begin(), response.headers.end(),
                       [](const auto& header) {
                           return httpHeaderName(header.first, "Content-Length") ||
                                  httpHeaderName(header.first, "Content-Encoding");
                       }),
        response.headers.end());
    response.headers.emplace_back("Content-Encoding", ruvia::httpContentCodingToken(coding));
    const auto vary = std::string(responseHeader(response, "Vary"));
    if (vary.empty()) {
        response.headers.emplace_back("Vary", "Accept-Encoding");
    } else if (vary.find("Accept-Encoding") == std::string::npos) {
        response.headers.erase(
            std::remove_if(response.headers.begin(), response.headers.end(),
                           [](const auto& header) { return httpHeaderName(header.first, "Vary"); }),
            response.headers.end());
        response.headers.emplace_back("Vary", vary + ", Accept-Encoding");
    }
}

} // namespace flexedge::node
