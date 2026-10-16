#pragma once

#include <ruvia/web/websocket_client.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "node/runtime/log_buffer.h"

namespace flexedge::node {

enum class channel_failure_kind {
    transient,
    permanent,
};

inline channel_failure_kind classify_channel_failure(ruvia::websocket_client_error::code_type code) {
    using code_type = ruvia::websocket_client_error::code_type;
    switch (code) {
        case code_type::timeout:
        case code_type::cancelled:
        case code_type::resolve_failed:
        case code_type::connect_failed:
        case code_type::handshake_rejected:
        case code_type::io_error:
        case code_type::closing:
            return channel_failure_kind::transient;
        case code_type::invalid_config:
        case code_type::invalid_state:
        case code_type::tls_failed:
        case code_type::protocol_error:
        case code_type::message_too_large:
            return channel_failure_kind::permanent;
    }
    return channel_failure_kind::permanent;
}

// Only an authenticated, acknowledged exchange resets the failure state;
// a new TCP connection or a normal batch close does not.
class channel_log final {
public:
    channel_log(node_log_buffer& logs, std::string category)
        : logs_(logs),
          category_(std::move(category)) {}

    void failed(std::string_view message, channel_failure_kind kind) {
        failure_count_ = (std::min)(failure_count_ + 1, std::size_t{6});
        const auto level = kind == channel_failure_kind::permanent ? "error" : "warning";
        report(level, "connection unavailable; reconnecting: " + std::string(message));
    }

    void failed(const ruvia::websocket_client_error& error) {
        auto kind = classify_channel_failure(error.code());
        // The pinned Ruvia version reports transport EOF without a Close frame
        // as protocol_error, without a public transport subcode. Keep this exact
        // diagnostic exception narrow so actual frame errors remain errors.
        if (error.code() == ruvia::websocket_client_error::code_type::protocol_error &&
            std::string_view(error.what()) == "WebSocket transport ended before peer Close") {
            kind = channel_failure_kind::transient;
        }
        failed(error.what(), kind);
    }

    void disconnected() {
        failed("control plane closed the connection", channel_failure_kind::transient);
    }

    void recovered() {
        const auto was_unavailable = failure_count_ != 0;
        failure_count_ = 0;
        if (was_unavailable) {
            report("info", "connection restored");
        }
    }

    [[nodiscard]] std::chrono::seconds retry_delay() const noexcept {
        const auto exponent = failure_count_ == 0 ? 0 : failure_count_ - 1;
        return std::chrono::seconds((std::min)(std::size_t{1} << exponent, std::size_t{30}));
    }

private:
    void report(std::string_view level, std::string_view message) {
        std::cerr << "flexedge node " << category_ << " channel " << level << ": " << message << '\n';
        logs_.node(level, category_, message);
    }

    node_log_buffer& logs_;
    std::string category_;
    std::size_t failure_count_{};
};

}  // namespace flexedge::node
