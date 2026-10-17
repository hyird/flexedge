#pragma once

#include <ruvia/http/http1_client_exchange_state.h>
#include <ruvia/http/http1_client_request_writer.h>
#include <ruvia/http/http1_close_policy.h>
#include <ruvia/http/http_client.h>
#include <ruvia/http/http_header.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/route_policy.h"
#include "node/data/http1_request_reader.h"
#include "node/data/route_rules.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

// `target_` borrows the request buffer or evaluated route through response handling.
// Wire bytes are owned. The writer result is private and retained only as the
// public Ruvia factory for independent response facts on each origin retry.
class prepared_origin_request final {
public:
    // Call only after serializing the writer's head/content views into `bytes`.
    prepared_origin_request(std::string bytes, std::string_view target,
        ruvia::prepared_http1_client_request prepared)
        : bytes_(std::move(bytes)),
          target_(target),
          prepared_(std::move(prepared)) {}

    [[nodiscard]] ruvia::http1_client_exchange_state make_exchange_state() const {
        return prepared_.exchange_state();
    }

    std::string bytes_;
    std::string_view target_;

private:
    // head()/content_plan() views expire at serialization; never read them here.
    // Ruvia's public exchange_state() factory reads only its owned protocol facts.
    ruvia::prepared_http1_client_request prepared_;
};

struct origin_request_view final {
    std::string_view method_;
    std::string_view target_;
    std::span<const ruvia::http_header_view> headers_;
    std::string_view body_;
    bool has_body_{};
};

inline bool accepts_event_stream(std::string_view value) noexcept {
    while (!value.empty()) {
        const auto separator = value.find(',');
        auto media_type = value.substr(0, separator);
        value =
            separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
        media_type = flexedge::route_policy::trim_ows(media_type.substr(0, media_type.find(';')));
        if (flexedge::route_policy::ascii_equal(media_type, "text/event-stream")) {
            return true;
        }
    }
    return false;
}

inline bool request_hop_by_hop_header(std::string_view name) noexcept {
    return flexedge::route_policy::ascii_equal(name, "Host") ||
           flexedge::route_policy::ascii_equal(name, "Connection") ||
           flexedge::route_policy::ascii_equal(name, "Content-Length") ||
           flexedge::route_policy::ascii_equal(name, "Transfer-Encoding") ||
           flexedge::route_policy::ascii_equal(name, "Expect") ||
           flexedge::route_policy::ascii_equal(name, "Keep-Alive") ||
           flexedge::route_policy::ascii_equal(name, "Proxy-Connection") ||
           flexedge::route_policy::ascii_equal(name, "TE") ||
           flexedge::route_policy::ascii_equal(name, "Trailer") ||
           flexedge::route_policy::ascii_equal(name, "Upgrade");
}

inline std::optional<prepared_origin_request>
prepare_origin_request(const origin_request_view& incoming, const v2::Website& website,
    std::string_view incoming_host, std::string_view client_address, bool secure,
    bool upgrade_requested, bool close_after_response,
    bool force_identity_encoding = false, const evaluated_route_policy* route = nullptr) {
    const auto authority =
        website.origin_host_header().empty() || website.origin_host_header() == "$host"
            ? flexedge::route_policy::hostname(incoming_host)
            : website.origin_host_header();
    // Request and compiled header buffers stay alive until the writer has serialized
    // the head; forwarding and mutations do not need owning string copies.
    std::vector<std::pair<std::string_view, std::string_view>> forwarded_headers;
    forwarded_headers.reserve(incoming.headers_.size() + 2 +
                              (route == nullptr ? 0 : route->request_headers().size()));
    for (const auto& header : incoming.headers_) {
        const auto upgrade_header =
            upgrade_requested && (flexedge::route_policy::ascii_equal(header.name(), "Connection") ||
                                     flexedge::route_policy::ascii_equal(header.name(), "Upgrade"));
        const auto encoding_header =
            force_identity_encoding &&
            flexedge::route_policy::ascii_equal(header.name(), "Accept-Encoding");
        if ((upgrade_header || !request_hop_by_hop_header(header.name())) && !encoding_header &&
            !(website.pass_client_ip() &&
                flexedge::route_policy::ascii_equal(header.name(), "X-Forwarded-For"))) {
            forwarded_headers.emplace_back(header.name(), header.value());
        }
    }
    if (force_identity_encoding) {
        forwarded_headers.emplace_back("Accept-Encoding", "identity");
    }
    if (website.pass_client_ip()) {
        forwarded_headers.emplace_back("X-Forwarded-For", client_address);
        forwarded_headers.emplace_back("X-Forwarded-Proto", secure ? "https" : "http");
    }
    if (route != nullptr) {
        apply_route_headers(forwarded_headers, route->request_headers());
    }
    std::vector<ruvia::http_header_view> headers;
    headers.reserve(forwarded_headers.size());
    for (const auto& header : forwarded_headers) {
        headers.emplace_back(header.first, header.second);
    }

    ruvia::http_client_request_view request;
    request.method_ = incoming.method_;
    const auto target = route ? route->rewritten_target() : incoming.target_;
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
    auto finish = [target](auto& prepared) -> std::optional<prepared_origin_request> {
        if (!prepared.prepared()) {
            return std::nullopt;
        }
        std::string bytes(prepared.prepared()->head());
        if (const auto* content = prepared.prepared()->content_plan().immediate()) {
            bytes.append(content->bytes());
        }
        return prepared_origin_request(
            std::move(bytes), target, std::move(*prepared.prepared()));
    };
    auto prepared = ruvia::http1_client_request_writer().prepare(origin, request, head, policy);
    if (const auto* small = prepared.buffer_too_small()) {
        head.resize(small->required_head_bytes());
        auto retry = ruvia::http1_client_request_writer().prepare(origin, request, head, policy);
        return finish(retry);
    }
    return finish(prepared);
}

inline std::optional<prepared_origin_request>
prepare_origin_request(const http1_request_reader& parsed, const v2::Website& website,
    std::string_view incoming_host, std::string_view client_address, bool secure,
    bool upgrade_requested, bool close_after_response,
    bool force_identity_encoding = false, const evaluated_route_policy* route = nullptr) {
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
        close_after_response, force_identity_encoding, route);
}

}  // namespace flexedge::node
