#pragma once

#include <stdexcept>
#include <string_view>
#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/runtime/secret_buffer.h"

namespace flexedge::node {

// Own the authentication envelope and serialized credential bytes for the
// entire exchange so both are cleansed on success, cancellation, or failure.
inline ruvia::Task<v2::Welcome> authenticateControlEnvelope(
    const ruvia::WebSocketClientHandle& client, v2::ClientEnvelope envelope,
    std::string_view failureMessage) {
    SecretStringGuard secretCleanser(*envelope.mutable_authenticate()->mutable_secret());
    auto bytes = serializeArtifact(envelope);
    SecretStringGuard bytesCleanser(bytes);
    co_await client.binary(bytes);
    const auto response = co_await readServerEnvelope(client);
    if (response.request_id() != envelope.request_id() || !response.has_welcome()) {
        throw std::runtime_error(std::string(failureMessage));
    }
    co_return response.welcome();
}

} // namespace flexedge::node
