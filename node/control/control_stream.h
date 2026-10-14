#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/websocket_client.h>

#include <exception>
#include <limits>
#include <stdexcept>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

class control_stream_ended final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "control stream ended";
    }
};

inline ruvia::task<v2::ServerEnvelope>
read_server_envelope(const ruvia::websocket_client_handle& connection) {
    const auto message = co_await connection.read();
    if (!message) {
        throw control_stream_ended{};
    }
    if (!message->binary()) {
        throw std::runtime_error("control plane must send protobuf binary messages");
    }
    if (message->payload().size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("control plane protobuf message is too large");
    }
    v2::ServerEnvelope envelope;
    if (!envelope.ParseFromArray(message->payload().data(),
            static_cast<int>(message->payload().size()))) {
        throw std::runtime_error("control plane sent invalid protobuf");
    }
    if (envelope.has_error()) {
        throw std::runtime_error("control plane error: " + envelope.error().message());
    }
    co_return envelope;
}

}  // namespace flexedge::node
