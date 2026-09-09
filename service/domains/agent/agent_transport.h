#pragma once
#include <cstddef>
#include <limits>
#include <ruvia/web/WebSocket.h>
#include "node/proto/edge_control.pb.h"
namespace service::agent {
inline bool parseClientEnvelope(const ruvia::WebSocketMessage& message,
                                flexedge::node::v2::ClientEnvelope& envelope) {
    if (!message.binary() ||
        message.payload().size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return envelope.ParseFromArray(message.payload().data(),
                                   static_cast<int>(message.payload().size()));
}

} // namespace service::agent
