#pragma once

#include <ruvia/http/http1_client_exchange_state.h>
#include <ruvia/http/http1_client_request_writer.h>
#include <ruvia/http/http1_close_policy.h>
#include <ruvia/http/http_client.h>
#include <ruvia/http/http_header.h>

#include <cctype>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/route_policy.h"
#include "node/data/http1_request_reader.h"
#include "node/data/route_rules.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

// `target_` is the request target after route rewriting.
struct prepared_origin_request final {
    std::string bytes_;
    ruvia::http1_client_exchange_state response_exchange_;
    std::string target_;
};

struct origin_request_view final {
    std::string_view method_;
    std::string_view target_;
    std::span<const ruvia::http_header_view> headers_;
    std::string_view body_;
    bool has_body_{};
};

inline bool http_header_name(std::string_view value, std::string_view expected) noexcept {
    if (value.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[index]);
        const auto right = static_cast<unsigned char>(expected[index]);
        if (std::tolower(left) != std::tolower(right)) {
            return false;
        }
    }
    return true;
}

inline bool accepts_event_stream(std::string_view value) noexcept {
    while (!value.empty()) {
        const auto separator = value.find(',');
        auto media_type = value.substr(0, separator);
        value =
            separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
        while (!media_type.empty() && (media_type.front() == ' ' || media_type.front() == '\t')) {
            media_type.remove_prefix(1);
        }
        const auto parameters = media_type.find(';');
        media_type = media_type.substr(0, parameters);
        while (!media_type.empty() && (media_type.back() == ' ' || media_type.back() == '\t')) {
            media_type.remove_suffix(1);
        }
        if (http_header_name(media_type, "text/event-stream")) {
            return true;
        }
    }
    return false;
}

inline bool request_hop_by_hop_header(std::string_view name) noexcept {
    return http_header_name(name, "Host") || http_header_name(name, "Connection") ||
           http_header_name(name, "Content-Length") || http_header_name(name, "Transfer-Encoding") ||
           http_header_name(name, "Expect") || http_header_name(name, "Keep-Alive") ||
           http_header_name(name, "Proxy-Connection") || http_header_name(name, "TE") ||
           http_header_name(name, "Trailer") || http_header_name(name, "Upgrade");
}

inline std::optional<prepared_origin_request>
prepare_origin_request(const origin_request_view& incoming, const v2::Website& website,
    std::string_view incoming_host, std::string_view client_address, bool secure,
    bool upgrade_requested, bool close_after_response,
    bool force_identity_encoding = false, const v2::RouteRule* route = nullptr,
    const route_patterns* patterns = nullptr) {
    const auto authority =
        website.origin_host_header().empty() || website.origin_host_header() == "$host"
            ? flexedge::route_policy::hostname(incoming_host)
            : website.origin_host_header();
    std::vector<std::pair<std::string, std::string>> owned_headers;
    owned_headers.reserve(incoming.headers_.size() + 2 +
                          (route == nullptr ? 0 : route->request_headers_size()));
    for (const auto& header : incoming.headers_) {
        const auto upgrade_header =
            upgrade_requested && (http_header_name(header.name(), "Connection") ||
                                     http_header_name(header.name(), "Upgrade"));
        const auto encoding_header =
            force_identity_encoding && http_header_name(header.name(), "Accept-Encoding");
        if ((upgrade_header || !request_hop_by_hop_header(header.name())) && !encoding_header &&
            !(website.pass_client_ip() && http_header_name(header.name(), "X-Forwarded-For"))) {
            owned_headers.emplace_back(header.name(), header.value());
        }
    }
    if (force_identity_encoding) {
        owned_headers.emplace_back("Accept-Encoding", "identity");
    }
    if (website.pass_client_ip()) {
        owned_headers.emplace_back("X-Forwarded-For", client_address);
        owned_headers.emplace_back("X-Forwarded-Proto", secure ? "https" : "http");
    }
    if (route != nullptr) {
        apply_route_headers(owned_headers, route->request_headers());
    }
    std::vector<ruvia::http_header_view> headers;
    headers.reserve(owned_headers.size());
    for (const auto& header : owned_headers) {
        headers.emplace_back(header.first, header.second);
    }

    ruvia::http_client_request_view request;
    request.method_ = incoming.method_;
    auto target = route_target(incoming.target_, route, patterns);
    if (target.empty()) {
        return std::nullopt;
    }
    request.target_ = target;
    request.headers_ = headers;
    if (incoming.has_body_) {
        request.content_ = ruvia::http_client_request_content_view::bytes(incoming.body_);
    }

    const auto origin = ruvia::http_origin_view::http({.host_ = authority});
    const auto policy = ruvia::http1_client_request_wire_policy{
        .close_policy_ = close_after_response ? ruvia::http1_close_policy::close_after_response
                                              : ruvia::http1_close_policy::allow_reuse};
    std::vector<char> head(16384);
    auto finish = [&target](const auto& prepared) -> std::optional<prepared_origin_request> {
        if (!prepared.prepared()) {
            return std::nullopt;
        }
        std::string bytes(prepared.prepared()->head());
        if (const auto* content = prepared.prepared()->content_plan().immediate()) {
            bytes.append(content->bytes());
        }
        return prepared_origin_request{.bytes_ = std::move(bytes),
            .response_exchange_ = prepared.prepared()->exchange_state(),
            .target_ = std::move(target)};
    };
    auto prepared = ruvia::http1_client_request_writer().prepare(origin, request, head, policy);
    if (const auto* small = prepared.buffer_too_small()) {
        head.resize(small->required_head_bytes());
        const auto retry = ruvia::http1_client_request_writer().prepare(origin, request, head, policy);
        return finish(retry);
    }
    return finish(prepared);
}

inline std::optional<prepared_origin_request>
prepare_origin_request(const http1_request_reader& parsed, const v2::Website& website,
    std::string_view incoming_host, std::string_view client_address, bool secure,
    bool upgrade_requested, bool close_after_response,
    bool force_identity_encoding = false, const v2::RouteRule* route = nullptr,
    const route_patterns* patterns = nullptr) {
    const auto body = parsed.body();
    if (!body) {
        return std::nullopt;
    }
    return prepare_origin_request({.method_ = parsed.request().method(),
                                      .target_ = parsed.request().target(),
                                      .headers_ = parsed.request().headers(),
                                      .body_ = *body,
                                      .has_body_ = parsed.has_body()},
        website, incoming_host, client_address, secure, upgrade_requested,
        close_after_response, force_identity_encoding, route, patterns);
}

}  // namespace flexedge::node
