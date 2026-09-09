#pragma once

#include "node/control/connect.h"
#include "node/control/log_delivery.h"
#include "node/control/authenticate.h"

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
#include <ruvia/core/Channel.h>
#include <ruvia/core/Timer.h>
#include <ruvia/core/StopToken.h>
#include <ruvia/web/WebSocketClient.h>

#include "node/control/control_stream.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
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
               NodeLogBuffer& logs, ruvia::ChannelReceiver<bool> queued)
        : loop_(std::move(loop)), config_(std::move(config)), credentials_(credentials),
          logs_(logs), queued_(std::move(queued)) {}

    ruvia::Task<void> run() {
        const auto worker = loop_.handle();
        auto retryDelay = std::chrono::seconds(1);
        while (!config_.stopToken.stopRequested()) {
            if (!logs_.pending()) {
                if (!(co_await queued_.receive(config_.stopToken)).hasValue()) {
                    co_return;
                }
                continue;
            }
            std::string errorText;
            try {
                ruvia::WebSocketClient client(loop_, config_.webSocket);
                co_await runSession(client);
                retryDelay = std::chrono::seconds(1);
                // A fully acknowledged batch is not a failed connection.
                // Return to queue readiness without imposing retry latency.
                continue;
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
        const auto welcome = co_await authenticateControlEnvelope(client, authenticateEnvelope(),
            "control plane did not authenticate log ingestion");
        if (welcome.node_id().empty()) {
            throw std::runtime_error("control plane did not authenticate log ingestion");
        }
        co_return welcome.node_id();
    }

    ruvia::Task<void> runSession(ruvia::WebSocketClient& client) {
        co_await connectControlTransport(client, config_.stopToken);
        if (client.subprotocol() != kControlSubprotocol) {
            throw std::runtime_error("control plane did not select the required subprotocol");
        }
        const auto connection = client.withOptions({.stopToken = config_.stopToken});
        const auto nodeId = co_await authenticate(connection);
        std::uint64_t sequence{};
        while (!config_.stopToken.stopRequested() && logs_.pending()) {
            co_await deliverLogs(connection, logs_, nodeId, ++sequence);
        }
    }

    ruvia::EventLoop loop_;
    LogChannelConfig config_;
    NodeCredentials& credentials_;
    NodeLogBuffer& logs_;
    ruvia::ChannelReceiver<bool> queued_;
};

} // namespace flexedge::node
