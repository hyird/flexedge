#pragma once

#include <cctype>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>
#include <ruvia/http/Http1RequestParser.h>

#include "node/data/chunked_body.h"
#include "node/data/route_rules.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/compiled_config.h"

namespace flexedge::node {

struct PreparedOriginRequest final {
    std::string bytes;
    ruvia::Http1ClientExchangeState responseExchange;
};

struct OriginRequestView final {
    std::string_view method;
    std::string_view target;
    std::span<const ruvia::HttpHeaderView> headers;
    std::string_view body;
    bool hasBody{};
};

inline bool httpHeaderName(std::string_view value, std::string_view expected) noexcept {
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

inline bool acceptsEventStream(std::string_view value) noexcept {
    while (!value.empty()) {
        const auto separator = value.find(',');
        auto mediaType = value.substr(0, separator);
        value =
            separator == std::string_view::npos ? std::string_view{} : value.substr(separator + 1);
        while (!mediaType.empty() && (mediaType.front() == ' ' || mediaType.front() == '\t')) {
            mediaType.remove_prefix(1);
        }
        const auto parameters = mediaType.find(';');
        mediaType = mediaType.substr(0, parameters);
        while (!mediaType.empty() && (mediaType.back() == ' ' || mediaType.back() == '\t')) {
            mediaType.remove_suffix(1);
        }
        if (httpHeaderName(mediaType, "text/event-stream")) {
            return true;
        }
    }
    return false;
}

inline bool requestHopByHopHeader(std::string_view name) noexcept {
    return httpHeaderName(name, "Host") || httpHeaderName(name, "Connection") ||
           httpHeaderName(name, "Content-Length") || httpHeaderName(name, "Transfer-Encoding") ||
           httpHeaderName(name, "Expect") || httpHeaderName(name, "Keep-Alive") ||
           httpHeaderName(name, "Proxy-Connection") || httpHeaderName(name, "TE") ||
           httpHeaderName(name, "Trailer") || httpHeaderName(name, "Upgrade");
}

inline std::optional<PreparedOriginRequest>
prepareOriginRequest(const OriginRequestView& incoming, const v2::Website& website,
                     std::string_view incomingHost, std::string_view clientAddress, bool secure,
                     bool upgradeRequested, bool closeAfterResponse,
                     bool forceIdentityEncoding = false, const v2::RouteRule* route = nullptr) {
    const auto authority =
        website.origin_host_header().empty() || website.origin_host_header() == "$host"
            ? normalizeHostname(incomingHost)
            : website.origin_host_header();
    std::vector<std::pair<std::string, std::string>> ownedHeaders;
    ownedHeaders.reserve(incoming.headers.size() + 2 +
                         (route == nullptr ? 0 : route->request_headers_size()));
    for (const auto& header : incoming.headers) {
        const auto upgradeHeader =
            upgradeRequested && (httpHeaderName(header.name(), "Connection") ||
                                 httpHeaderName(header.name(), "Upgrade"));
        const auto encodingHeader =
            forceIdentityEncoding && httpHeaderName(header.name(), "Accept-Encoding");
        if ((upgradeHeader || !requestHopByHopHeader(header.name())) && !encodingHeader &&
            !(website.pass_client_ip() && httpHeaderName(header.name(), "X-Forwarded-For"))) {
            ownedHeaders.emplace_back(header.name(), header.value());
        }
    }
    if (forceIdentityEncoding) {
        ownedHeaders.emplace_back("Accept-Encoding", "identity");
    }
    if (website.pass_client_ip()) {
        ownedHeaders.emplace_back("X-Forwarded-For", clientAddress);
        ownedHeaders.emplace_back("X-Forwarded-Proto", secure ? "https" : "http");
    }
    if (route != nullptr) {
        applyRouteHeaders(ownedHeaders, route->request_headers());
    }
    std::vector<ruvia::HttpHeaderView> headers;
    headers.reserve(ownedHeaders.size());
    for (const auto& header : ownedHeaders) {
        headers.emplace_back(header.first, header.second);
    }

    ruvia::HttpClientRequestView request;
    request.method = incoming.method;
    const auto target = routeTarget(incoming.target, route);
    request.target = target;
    request.headers = headers;
    if (incoming.hasBody) {
        request.content = ruvia::HttpClientRequestContentView::bytes(incoming.body);
    }

    const auto origin = ruvia::HttpOriginView::http({.host = authority});
    const auto policy = ruvia::Http1ClientRequestWirePolicy{
        .closePolicy = closeAfterResponse ? ruvia::Http1ClosePolicy::kCloseAfterResponse
                                          : ruvia::Http1ClosePolicy::kAllowReuse};
    std::vector<char> head(16384);
    auto finish = [](const auto& prepared) -> std::optional<PreparedOriginRequest> {
        if (!prepared.prepared()) {
            return std::nullopt;
        }
        std::string bytes(prepared.prepared()->head());
        if (const auto* content = prepared.prepared()->contentPlan().immediate()) {
            bytes.append(content->bytes());
        }
        return PreparedOriginRequest{.bytes = std::move(bytes),
                                     .responseExchange = prepared.prepared()->exchangeState()};
    };
    auto prepared = ruvia::Http1ClientRequestWriter().prepare(origin, request, head, policy);
    if (const auto* small = prepared.bufferTooSmall()) {
        head.resize(small->requiredHeadBytes());
        const auto retry = ruvia::Http1ClientRequestWriter().prepare(origin, request, head, policy);
        return finish(retry);
    }
    return finish(prepared);
}

inline std::optional<PreparedOriginRequest>
prepareOriginRequest(const ruvia::Http1ParsedRequest& parsed, const v2::Website& website,
                     std::string_view incomingHost, std::string_view clientAddress, bool secure,
                     bool upgradeRequested, bool closeAfterResponse, std::size_t maxRequestBytes,
                     bool forceIdentityEncoding = false, const v2::RouteRule* route = nullptr) {
    std::string decodedBody;
    std::string_view body;
    bool hasBody = false;
    if (parsed.bodyPlan().chunked()) {
        auto decoded = decodeChunkedBody(parsed.wireBody(), maxRequestBytes);
        if (!decoded) {
            return std::nullopt;
        }
        decodedBody = std::move(*decoded);
        body = decodedBody;
        hasBody = true;
    } else if (parsed.bodyPlan().knownLength()) {
        body = parsed.wireBody();
        hasBody = true;
    }
    return prepareOriginRequest({.method = parsed.request().method(),
                                 .target = parsed.request().target(),
                                 .headers = parsed.request().headers(),
                                 .body = body,
                                 .hasBody = hasBody},
                                website, incomingHost, clientAddress, secure, upgradeRequested,
                                closeAfterResponse, forceIdentityEncoding, route);
}

} // namespace flexedge::node
