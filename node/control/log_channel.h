#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/web/WebSocketClient.h>

#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/secret_buffer.h"
#include "node/runtime/version.h"

namespace flexedge::node {

struct LogChannelConfig final {
    ruvia::WebSocketClientConfig webSocket;
    ruvia::StopToken stopToken;
    std::string agentVersion{std::string(kNodeVersion)};
};

class LogChannel final {
  public:
    LogChannel(ruvia::EventLoop loop, LogChannelConfig config, NodeCredentials& credentials,
               NodeLogBuffer& logs)
        : loop_(std::move(loop)), config_(std::move(config)), credentials_(credentials),
          logs_(logs) {}

    ruvia::Task<void> run() {
        const auto worker = loop_.handle();
        auto retryDelay = std::chrono::seconds(1);
        while (!config_.stopToken.stopRequested()) {
            if (!logs_.pending()) {
                if (co_await ruvia::sleepFor(worker, std::chrono::milliseconds(10),
                                             config_.stopToken) ==
                    ruvia::TimerSleepResult::kStopRequested) {
                    co_return;
                }
                continue;
            }
            std::string errorText;
            try {
                ruvia::WebSocketClient client(loop_, config_.webSocket);
                co_await runSession(client);
                retryDelay = std::chrono::seconds(1);
            } catch (const ControlStreamEnded&) {
                retryDelay = std::chrono::seconds(1);
            } catch (const ruvia::WebSocketClientError& error) {
                errorText = error.what();
            } catch (const std::exception& error) {
                errorText = error.what();
            }
            if (config_.stopToken.stopRequested()) {
                co_return;
            }
            if (!errorText.empty()) {
                std::cerr << "flexedge node log channel failed: " << errorText << '\n';
                logs_.node("error", "log-ingest", errorText);
            }
            const auto slept = co_await ruvia::sleepFor(worker, retryDelay, config_.stopToken);
            if (slept == ruvia::TimerSleepResult::kStopRequested) {
                co_return;
            }
            retryDelay = (std::min)(retryDelay * 2, std::chrono::seconds(30));
        }
    }

  private:
    static std::string serialize(const v2::ClientEnvelope& envelope) {
        return serializeArtifact(envelope);
    }

    v2::ClientEnvelope authenticateEnvelope() const {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("log-authenticate");
        auto* value = envelope.mutable_authenticate();
        value->set_node_id(credentials_.nodeId());
        value->set_secret(credentials_.secret());
        value->set_agent_version(config_.agentVersion);
        value->set_session_purpose(v2::AGENT_SESSION_PURPOSE_LOG_INGEST);
        return envelope;
    }

    ruvia::Task<std::string> authenticate(const ruvia::WebSocketClientHandle& client) {
        auto envelope = authenticateEnvelope();
        auto* secret = envelope.mutable_authenticate()->mutable_secret();
        SecretStringGuard secretCleanser(*secret);
        auto bytes = serialize(envelope);
        SecretStringGuard bytesCleanser(bytes);
        co_await client.binary(bytes);
        const auto welcomeEnvelope = co_await readServerEnvelope(client);
        if (welcomeEnvelope.request_id() != envelope.request_id() ||
            !welcomeEnvelope.has_welcome() || welcomeEnvelope.welcome().node_id().empty()) {
            throw std::runtime_error("control plane did not authenticate log ingestion");
        }
        co_return welcomeEnvelope.welcome().node_id();
    }

    ruvia::Task<void> deliver(const ruvia::WebSocketClientHandle& client, std::string_view nodeId,
                              std::uint64_t sequence) {
        auto delivery = logs_.take(nodeId);
        if (!delivery) {
            co_return;
        }
        try {
            v2::ClientEnvelope envelope;
            const auto requestId = "logs-" + std::to_string(sequence);
            envelope.set_request_id(requestId);
            *envelope.mutable_log_delivery() = delivery->value();
            co_await client.binary(serialize(envelope));
            const auto acknowledgement = co_await readServerEnvelope(client);
            if (acknowledgement.request_id() != requestId ||
                !acknowledgement.has_log_delivery_ack()) {
                throw std::runtime_error("control plane did not acknowledge log delivery");
            }
            logs_.acknowledge(std::move(*delivery));
        } catch (...) {
            logs_.restore(std::move(*delivery));
            throw;
        }
    }

    ruvia::Task<void> runSession(ruvia::WebSocketClient& client) {
        co_await client.connect();
        if (client.subprotocol() != kControlSubprotocol) {
            throw std::runtime_error("control plane did not select the required subprotocol");
        }
        const auto connection = client.withOptions({.stopToken = config_.stopToken});
        const auto nodeId = co_await authenticate(connection);
        std::uint64_t sequence{};
        while (!config_.stopToken.stopRequested() && logs_.pending()) {
            co_await deliver(connection, nodeId, ++sequence);
        }
    }

    ruvia::EventLoop loop_;
    LogChannelConfig config_;
    NodeCredentials& credentials_;
    NodeLogBuffer& logs_;
};

} // namespace flexedge::node
