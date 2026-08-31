#pragma once

#include <exception>
#include <limits>
#include <stdexcept>

#include <ruvia/core/Task.h>
#include <ruvia/web/WebSocketClient.h>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

class ControlStreamEnded final : public std::exception {
  public:
    [[nodiscard]] const char* what() const noexcept override { return "control stream ended"; }
};

inline ruvia::Task<v2::ServerEnvelope>
readServerEnvelope(const ruvia::WebSocketClientHandle& connection) {
    const auto message = co_await connection.read();
    if (!message) {
        throw ControlStreamEnded{};
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

} // namespace flexedge::node
