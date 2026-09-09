#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

inline ruvia::Task<void> deliverLogs(const ruvia::WebSocketClientHandle& client, NodeLogBuffer& logs, std::string_view nodeId,
                          std::uint64_t sequence) {
    auto delivery = logs.take(nodeId);
    if (!delivery) {
        co_return;
    }
    try {
        v2::ClientEnvelope envelope;
        const auto requestId = "logs-" + std::to_string(sequence);
        envelope.set_request_id(requestId);
        *envelope.mutable_log_delivery() = delivery->value();
        co_await client.binary(serializeArtifact(envelope));
        const auto acknowledgement = co_await readServerEnvelope(client);
        if (acknowledgement.request_id() != requestId ||
            !acknowledgement.has_log_delivery_ack()) {
            throw std::runtime_error("control plane did not acknowledge log delivery");
        }
        logs.acknowledge(std::move(*delivery));
    } catch (...) {
        logs.restore(std::move(*delivery));
        throw;
    }
}

} // namespace flexedge::node
