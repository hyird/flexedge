#pragma once

#include <ruvia/core/StopToken.h>
#include <ruvia/web/WebSocketClient.h>

namespace flexedge::node {

// OperationOptions applies after connection establishment. Bridge process
// cancellation to the transport while DNS/TCP/TLS/WebSocket setup is pending.
inline ruvia::Task<void> connectControlTransport(ruvia::WebSocketClient& client,
                                               ruvia::StopToken stopToken) {
    auto cancellation = stopToken.registerCallback([&client] { client.abort(); });
    if (stopToken.stopRequested()) co_return;
    co_await client.connect();
}

} // namespace flexedge::node
