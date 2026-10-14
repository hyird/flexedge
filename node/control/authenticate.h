#pragma once

#include <stdexcept>
#include <string_view>

#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/runtime/secret_buffer.h"

namespace flexedge::node {

// Own the authentication envelope and serialized credential bytes for the
// entire exchange so both are cleansed on success, cancellation, or failure.
inline ruvia::task<v2::Welcome> authenticate_control_envelope(
    const ruvia::websocket_client_handle& client, v2::ClientEnvelope envelope,
    std::string_view failure_message) {
    secret_string_guard secret_cleanser(*envelope.mutable_authenticate()->mutable_secret());
    auto bytes = serialize_artifact(envelope);
    secret_string_guard bytes_cleanser(bytes);
    co_await client.binary(bytes);
    const auto response = co_await read_server_envelope(client);
    if (response.request_id() != envelope.request_id() || !response.has_welcome()) {
        throw std::runtime_error(std::string(failure_message));
    }
    co_return response.welcome();
}

}  // namespace flexedge::node
