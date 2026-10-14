#pragma once
#include <ruvia/web/websocket.h>

#include <cstddef>
#include <limits>

#include "node/proto/edge_control.pb.h"
namespace service::agent {
inline bool parse_client_envelope(const ruvia::websocket_message& message,
    flexedge::node::v2::ClientEnvelope& envelope) {
    if (!message.binary() ||
        message.payload().size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return envelope.ParseFromArray(message.payload().data(),
        static_cast<int>(message.payload().size()));
}

}  // namespace service::agent
