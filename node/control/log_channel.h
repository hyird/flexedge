#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/websocket_client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "node/control/authenticate.h"
#include "node/control/connect.h"
#include "node/control/control_stream.h"
#include "node/control/log_delivery.h"
#include "node/proto/artifact.h"
#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/version.h"

namespace flexedge::node {

struct log_channel_config final {
    ruvia::websocket_client_config web_socket_;
    ruvia::stop_token stop_token_;
    std::string agent_version_{std::string(node_version)};
};

class log_channel final {
public:
    log_channel(ruvia::event_loop loop, log_channel_config config, node_credentials& credentials,
        node_log_buffer& logs, ruvia::channel_receiver<bool> queued)
        : loop_(std::move(loop)),
          config_(std::move(config)),
          credentials_(credentials),
          logs_(logs),
          queued_(std::move(queued)) {}

    ruvia::task<void> run() {
        const auto worker = loop_.handle();
        auto retry_delay = std::chrono::seconds(1);
        while (!config_.stop_token_.stop_requested()) {
            if (!logs_.pending()) {
                if (!(co_await queued_.receive(config_.stop_token_)).has_value()) {
                    co_return;
                }
                continue;
            }
            std::string error_text;
            try {
                ruvia::websocket_client client(loop_, config_.web_socket_);
                co_await run_session(client);
                retry_delay = std::chrono::seconds(1);
                // A fully acknowledged batch is not a failed connection.
                // Return to queue readiness without imposing retry latency.
                continue;
            } catch (const control_stream_ended&) {
                retry_delay = std::chrono::seconds(1);
            } catch (const ruvia::websocket_client_error& error) {
                error_text = error.what();
            } catch (const std::exception& error) {
                error_text = error.what();
            }
            if (config_.stop_token_.stop_requested()) {
                co_return;
            }
            if (!error_text.empty()) {
                std::cerr << "flexedge node log channel failed: " << error_text << '\n';
                logs_.node("error", "log-ingest", error_text);
            }
            const auto slept = co_await ruvia::sleep_for(worker, retry_delay, config_.stop_token_);
            if (slept == ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
            retry_delay = (std::min)(retry_delay * 2, std::chrono::seconds(30));
        }
    }

private:
    v2::ClientEnvelope authenticate_envelope() const {
        v2::ClientEnvelope envelope;
        envelope.set_request_id("log-authenticate");
        auto* value = envelope.mutable_authenticate();
        value->set_node_id(credentials_.node_id());
        value->set_secret(credentials_.secret());
        value->set_agent_version(config_.agent_version_);
        value->set_session_purpose(v2::AGENT_SESSION_PURPOSE_LOG_INGEST);
        return envelope;
    }

    ruvia::task<std::string> authenticate(const ruvia::websocket_client_handle& client) {
        const auto welcome = co_await authenticate_control_envelope(client, authenticate_envelope(),
            "control plane did not authenticate log ingestion");
        if (welcome.node_id().empty()) {
            throw std::runtime_error("control plane did not authenticate log ingestion");
        }
        co_return welcome.node_id();
    }

    ruvia::task<void> run_session(ruvia::websocket_client& client) {
        co_await connect_control_transport(client, config_.stop_token_);
        if (client.subprotocol() != control_subprotocol) {
            throw std::runtime_error("control plane did not select the required subprotocol");
        }
        const auto connection = client.with_options({.stop_token_ = config_.stop_token_});
        const auto node_id = co_await authenticate(connection);
        std::uint64_t sequence{};
        while (!config_.stop_token_.stop_requested() && logs_.pending()) {
            co_await deliver_logs(connection, logs_, node_id, ++sequence);
        }
    }

    ruvia::event_loop loop_;
    log_channel_config config_;
    node_credentials& credentials_;
    node_log_buffer& logs_;
    ruvia::channel_receiver<bool> queued_;
};

}  // namespace flexedge::node
