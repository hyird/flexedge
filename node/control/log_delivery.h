#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

inline ruvia::task<void> deliver_logs(const ruvia::websocket_client_handle& client, node_log_buffer& logs, std::string_view node_id,
    std::uint64_t sequence) {
    auto delivery = logs.take(node_id);
    if (!delivery) {
        co_return;
    }
    try {
        v2::ClientEnvelope envelope;
        const auto request_id = "logs-" + std::to_string(sequence);
        envelope.set_request_id(request_id);
        *envelope.mutable_log_delivery() = delivery->value();
        co_await client.binary(serialize_artifact(envelope));
        const auto acknowledgement = co_await read_server_envelope(client);
        if (acknowledgement.request_id() != request_id ||
            !acknowledgement.has_log_delivery_ack()) {
            throw std::runtime_error("control plane did not acknowledge log delivery");
        }
        logs.acknowledge(std::move(*delivery));
    } catch (...) {
        logs.restore(std::move(*delivery));
        throw;
    }
}

}  // namespace flexedge::node
