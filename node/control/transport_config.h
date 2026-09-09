#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <system_error>

#include <asio/ip/address_v6.hpp>
#include <ruvia/web/WebSocketClient.h>
#include "node/proto/control_protocol.h"
#include "node/runtime/version.h"

namespace flexedge::node {

inline ruvia::WebSocketClientConfig controlTransportConfig(std::string_view address) {
    auto scheme = ruvia::WebSocketScheme::kWss;
    if (address.starts_with("wss://")) {
        address.remove_prefix(6);
    } else if (address.starts_with("ws://")) {
        scheme = ruvia::WebSocketScheme::kWs;
        address.remove_prefix(5);
    } else if (address.contains("://")) {
        throw std::runtime_error("server scheme must be ws or wss");
    }
    if (address.empty() || address.contains('@') || address.contains('?') ||
        address.contains('#') || address.contains('\\') ||
        std::ranges::any_of(address, [](unsigned char c) { return c <= 0x20 || c == 0x7f; })) {
        throw std::runtime_error("server address is invalid");
    }
    auto authority = address;
    std::string target{"/api/agent/connect"};
    if (const auto slash = address.find('/'); slash != std::string_view::npos) {
        authority = address.substr(0, slash);
        target = std::string(address.substr(slash));
    }
    std::string host;
    std::optional<std::uint16_t> port;
    std::string_view portText;
    bool hasPort = false;
    if (authority.starts_with('[')) {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) {
            throw std::runtime_error("server address is invalid");
        }
        host = std::string(authority.substr(1, closing - 1));
        std::error_code addressError;
        (void)asio::ip::make_address_v6(host, addressError);
        if (addressError) {
            throw std::runtime_error("server bracketed host must be an IPv6 address");
        }
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') {
                throw std::runtime_error("server address is invalid");
            }
            portText = authority.substr(closing + 2);
            hasPort = true;
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = std::string(authority.substr(0, colon));
        portText = authority.substr(colon + 1);
        hasPort = true;
        if (host.contains(':')) {
            throw std::runtime_error("server IPv6 address must be bracketed");
        }
    } else {
        host = std::string(authority);
    }
    if (host.empty()) {
        throw std::runtime_error("server domain is required");
    }
    if (host.contains('[') || host.contains(']')) {
        throw std::runtime_error("server address is invalid");
    }
    if (hasPort) {
        if (portText.empty()) throw std::runtime_error("server port is required after colon");
        unsigned int parsed{};
        const auto result =
            std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != portText.data() + portText.size() ||
            parsed == 0 || parsed > 65535) {
            throw std::runtime_error("server port is invalid");
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    return {
        .scheme = scheme,
        .host = std::move(host),
        .port = port,
        .target = std::move(target),
        .subprotocols = {std::string(flexedge::node::kControlSubprotocol)},
        .maxMessageBytes = 4 * 1024 * 1024,
        .connectTimeout = std::chrono::seconds(10),
        .readTimeout = std::chrono::seconds(5),
        .writeTimeout = std::chrono::seconds(30),
        .closeHandshakeTimeout = std::chrono::seconds(5),
        .userAgent = flexedge::node::nodeUserAgent(),
    };
}

} // namespace flexedge::node
