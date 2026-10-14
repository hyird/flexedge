#pragma once

#include <ruvia/web/websocket_client.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/ip/address_v6.hpp>

#include "node/proto/control_protocol.h"
#include "node/runtime/version.h"

namespace flexedge::node {

inline ruvia::websocket_client_config control_transport_config(std::string_view address) {
    auto scheme = ruvia::websocket_scheme::wss;
    if (address.starts_with("wss://")) {
        address.remove_prefix(6);
    } else if (address.starts_with("ws://")) {
        scheme = ruvia::websocket_scheme::ws;
        address.remove_prefix(5);
    } else if (address.find("://") != std::string_view::npos) {
        throw std::runtime_error("server scheme must be ws or wss");
    }
    if (address.empty() || address.find('@') != std::string_view::npos ||
        address.find('?') != std::string_view::npos ||
        address.find('#') != std::string_view::npos ||
        address.find('\\') != std::string_view::npos ||
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
    std::string_view port_text;
    bool has_port = false;
    if (authority.starts_with('[')) {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) {
            throw std::runtime_error("server address is invalid");
        }
        host = std::string(authority.substr(1, closing - 1));
        std::error_code address_error;
        (void)asio::ip::make_address_v6(host, address_error);
        if (address_error) {
            throw std::runtime_error("server bracketed host must be an IPv6 address");
        }
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') {
                throw std::runtime_error("server address is invalid");
            }
            port_text = authority.substr(closing + 2);
            has_port = true;
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = std::string(authority.substr(0, colon));
        port_text = authority.substr(colon + 1);
        has_port = true;
        if (host.find(':') != std::string::npos) {
            throw std::runtime_error("server IPv6 address must be bracketed");
        }
    } else {
        host = std::string(authority);
    }
    if (host.empty()) {
        throw std::runtime_error("server domain is required");
    }
    if (host.find('[') != std::string::npos || host.find(']') != std::string::npos) {
        throw std::runtime_error("server address is invalid");
    }
    if (has_port) {
        if (port_text.empty()) {
            throw std::runtime_error("server port is required after colon");
        }
        unsigned int parsed{};
        const auto result =
            std::from_chars(port_text.data(), port_text.data() + port_text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != port_text.data() + port_text.size() ||
            parsed == 0 || parsed > 65535) {
            throw std::runtime_error("server port is invalid");
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    return {
        .scheme_ = scheme,
        .host_ = std::move(host),
        .port_ = port,
        .target_ = std::move(target),
        .subprotocols_ = {std::string(flexedge::node::control_subprotocol)},
        .max_message_bytes_ = 4 * 1024 * 1024,
        .connect_timeout_ = std::chrono::seconds(10),
        .read_timeout_ = std::chrono::seconds(5),
        .write_timeout_ = std::chrono::seconds(30),
        .close_handshake_timeout_ = std::chrono::seconds(5),
        .user_agent_ = flexedge::node::node_user_agent(),
    };
}

}  // namespace flexedge::node
