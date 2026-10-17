#pragma once

#include <ruvia/http/http_content_encoder.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "common/route_policy.h"
#include "node/data/buffered_bytes_budget.h"
#include "node/data/http_content_type.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline bool matches_compression_mime(const v2::Website& website, std::string_view value) noexcept {
    const auto semicolon = value.find(';');
    value = flexedge::route_policy::trim_ows(value.substr(0, semicolon));
    for (const auto& configured : website.response_compression_mime_types()) {
        const std::string_view pattern(configured);
        if (pattern.ends_with("/*")) {
            if (flexedge::route_policy::ascii_starts_with(value, pattern.substr(0, pattern.size() - 1))) {
                return true;
            }
        } else if (flexedge::route_policy::ascii_equal(value, pattern)) {
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
        return flexedge::route_policy::ascii_equal(extension, std::string_view(value));
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

struct encoding_quality_item final {
    std::string_view token_;
    double quality_{1};
};

inline encoding_quality_item parse_encoding_quality_item(std::string_view item) noexcept {
    item = flexedge::route_policy::trim_ows(item);
    const auto semicolon = item.find(';');
    const auto token = flexedge::route_policy::trim_ows(item.substr(0, semicolon));
    double quality = 1;
    if (semicolon != std::string_view::npos) {
        const auto parameters = item.substr(semicolon + 1);
        const auto equal = parameters.find('=');
        if (equal != std::string_view::npos) {
            const auto value = flexedge::route_policy::trim_ows(parameters.substr(equal + 1));
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), quality);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                quality < 0 || quality > 1) {
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
        if (flexedge::route_policy::ascii_equal(parsed.token_, wanted)) {
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

struct response_compression_choice final {
    ruvia::http_content_coding coding_{ruvia::http_content_coding::identity};
    bool vary_{};
};

inline ruvia::http_content_coding preferred_response_coding(
    const v2::Website& website, std::string_view accept_encoding) noexcept {
    auto coding = ruvia::http_content_coding::identity;
    double quality = 0;
    if (website.response_compression_enabled()) {
        for (const auto& configured : website.response_compression_algorithms()) {
            const auto candidate = configured_coding(configured);
            if (candidate) {
                const auto candidate_quality = encoding_quality(
                    accept_encoding, ruvia::http_content_coding_token(*candidate));
                if (candidate_quality > quality) {
                    coding = *candidate;
                    quality = candidate_quality;
                }
            }
        }
    }
    return coding;
}

// Unknown-length responses are streamed without buffering to discover their size.
// Size limits apply only when the origin supplies a representation length.
inline response_compression_choice select_response_compression(const v2::Website& website,
    std::string_view target, std::string_view accept_encoding, std::uint16_t status,
    std::optional<std::size_t> length, const auto& header) {
    if (!website.response_compression_enabled() || status == 206 ||
        is_event_stream_content_type(header("Content-Type")) ||
        (length && (*length == 0 || *length < website.response_compression_min_bytes() ||
                       (website.response_compression_max_bytes() != 0 &&
                           *length > website.response_compression_max_bytes())))) {
        return {};
    }
    const auto extension = request_extension(target);
    if (!extension.empty() && matches_compression_extension(
                                  website.response_compression_excluded_extensions(), extension)) {
        return {};
    }
    if (!matches_compression_mime(website, header("Content-Type")) &&
        (extension.empty() ||
            !matches_compression_extension(website.response_compression_extensions(), extension))) {
        return {};
    }
    const auto encoding = header("Content-Encoding");
    if ((!encoding.empty() && !flexedge::route_policy::ascii_equal(encoding, "identity")) ||
        !header("Content-Range").empty()) {
        return {};
    }
    const auto control_value = header("Cache-Control");
    auto control = std::string_view(control_value);
    while (!control.empty()) {
        const auto comma = control.find(',');
        if (flexedge::route_policy::ascii_equal(
                flexedge::route_policy::trim_ows(control.substr(0, comma)), "no-transform")) {
            return {};
        }
        if (comma == std::string_view::npos) {
            break;
        }
        control.remove_prefix(comma + 1);
    }
    return {.coding_ = preferred_response_coding(website, accept_encoding), .vary_ = true};
}

// Stable-address, worker-owned incremental encoder. The lease accounts for codec
// working memory and output buffers, independently of any optional cache capture.
class response_compressor final {
public:
    response_compressor(ruvia::http_content_coding coding, buffered_bytes_lease reservation)
        : memory_(std::move(reservation)),
          encoder_(coding, &memory_),
          output_(&memory_) {
        encoder_.write({}, output_, true);
    }
    [[nodiscard]] std::string_view write(std::string_view bytes) {
        encoder_.write(bytes, output_, true);
        return output_;
    }
    [[nodiscard]] std::string_view finish() {
        encoder_.finish(output_);
        return output_;
    }
    void clear_output() {
        output_.clear();
    }

private:
    class memory_resource final : public std::pmr::memory_resource {
    public:
        explicit memory_resource(buffered_bytes_lease reservation)
            : reservation_(std::move(reservation)) {}

    private:
        void* do_allocate(std::size_t bytes, std::size_t alignment) override {
            if (!reservation_.try_grow(bytes)) {
                throw std::bad_alloc();
            }
            try {
                return std::pmr::new_delete_resource()->allocate(bytes, alignment);
            } catch (...) {
                reservation_.shrink(bytes);
                throw;
            }
        }
        void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
            reservation_.shrink(bytes);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
        buffered_bytes_lease reservation_;
    };
    memory_resource memory_;
    ruvia::http_content_encoder encoder_;
    std::pmr::string output_;
};

}  // namespace flexedge::node
