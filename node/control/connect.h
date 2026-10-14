#pragma once

#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/web/websocket_client.h>

namespace flexedge::node {

// OperationOptions applies after connection establishment. Bridge process
// cancellation to the transport while DNS/TCP/TLS/WebSocket setup is pending.
inline ruvia::task<void> connect_control_transport(ruvia::websocket_client& client,
    ruvia::stop_token stop_token) {
    auto cancellation = stop_token.register_callback([&client] { client.abort(); });
    if (stop_token.stop_requested()) {
        co_return;
    }
    co_await client.connect();
}

}  // namespace flexedge::node
