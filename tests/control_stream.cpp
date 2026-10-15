#include "node/control/control_stream.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/http/http1_server_request_parser.h>
#include <ruvia/http/websocket_handshake.h>
#include <ruvia/web/websocket_client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "node/control/authenticate.h"
#include "node/control/log_delivery.h"
#include "node/control/transport_config.h"
#include "node/proto/control_protocol.h"
#include "node/proto/control_reply.h"
#include "service/features/live_resource/notification_reader.h"
#include "service/features/node_dispatch/fanout.h"

namespace {

enum class expected_result : std::uint8_t {
    envelope,
    stream_ended,
    protocol_error,
    authenticated,
    authentication_rejected,
    delivered,
    delivery_rejected,
};

class web_socket_origin final {
public:
    web_socket_origin(std::uint8_t opcode, std::string payload, bool authentication = false)
        : opcode_(opcode),
          payload_(std::move(payload)),
          authentication_(authentication),
          acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { serve(); }) {}

    ~web_socket_origin() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            const std::string request(asio::buffers_begin(buffer.data()),
                asio::buffers_end(buffer.data()));
            ruvia::http1_server_request_parse_state parsed;
            ruvia::http1_server_request_parser().parse_head(request, parsed);
            if (parsed.head_ready() == nullptr) {
                return;
            }
            const std::array<std::string_view, 1> subprotocols{flexedge::node::control_subprotocol};
            const auto validation = ruvia::validate_websocket_handshake(
                parsed.request_, parsed.body_plan_,
                {.supported_subprotocols_ = subprotocols, .require_subprotocol_ = true});
            if (validation.accepted() == nullptr) {
                return;
            }
            const auto handshake = ruvia::make_websocket_server_handshake(
                parsed.request_, {.supported_subprotocols_ = subprotocols});
            std::string response;
            handshake.for_each_response_part(
                [&response](std::string_view part) { response.append(part); });
            asio::write(socket, asio::buffer(response));
            if (authentication_) {
                // Wait for the authentication send before replying. The client
                // must exercise binary() as well as welcome validation.
                std::array<char, 4096> incoming{};
                (void)socket.read_some(asio::buffer(incoming));
            }

            std::string frame;
            frame.push_back(static_cast<char>(0x80U | opcode_));
            frame.push_back(static_cast<char>(payload_.size()));
            frame.append(payload_);
            asio::write(socket, asio::buffer(frame));
        } catch (...) {
        }
    }

    std::uint8_t opcode_;
    std::string payload_;
    bool authentication_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

ruvia::task<int> exercise(ruvia::websocket_client& client, expected_result expected) {
    co_await client.connect();
    const auto connection = client.with_options({});
    try {
        if (expected == expected_result::delivered || expected == expected_result::delivery_rejected) {
            flexedge::node::node_log_buffer logs;
            (void)logs.node("info", "test", "retained until acknowledged");
            try {
                co_await flexedge::node::deliver_logs(connection, logs, "node", 1);
                co_return expected == expected_result::delivered&& logs.queued_events() == 0 && logs.retained_events() == 0 ? 0 : 6;
            } catch (const std::runtime_error& error) {
                const auto retry = logs.take("node");
                co_return expected == expected_result::delivery_rejected&& std::string_view(error.what()) == "control plane did not acknowledge log delivery" &&
                        logs.queued_events() == 1 && logs.retained_events() == 1 && retry.has_value()
                    ? 0
                    : 7;
            }
        }
        if (expected == expected_result::authenticated ||
            expected == expected_result::authentication_rejected) {
            flexedge::node::v2::ClientEnvelope authentication;
            authentication.set_request_id("auth");
            authentication.mutable_authenticate()->set_node_id("node");
            authentication.mutable_authenticate()->set_secret("synthetic-secret");
            try {
                const auto welcome = co_await flexedge::node::authenticate_control_envelope(
                    connection, std::move(authentication), "authentication rejected");
                co_return expected == expected_result::authenticated&& welcome.node_id() == "node" ? 0 : 4;
            } catch (const std::runtime_error& error) {
                co_return expected == expected_result::authentication_rejected&& std::string_view(error.what()) == "authentication rejected" ? 0 : 5;
            }
        }
        const auto envelope = co_await flexedge::node::read_server_envelope(connection);
        const bool matched = expected == expected_result::envelope &&
                             envelope.has_heartbeat_ack() &&
                             envelope.heartbeat_ack().node_binary_sha256() ==
                                 "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea";
        co_return matched ? 0 : 1;
    } catch (const flexedge::node::control_stream_ended&) {
        co_return expected == expected_result::stream_ended ? 0 : 2;
    } catch (const std::runtime_error& error) {
        const bool matched =
            expected == expected_result::protocol_error &&
            std::string_view(error.what()) == "control plane must send protobuf binary messages";
        co_return matched ? 0 : 3;
    }
}

int run_case(std::uint8_t opcode, std::string payload, expected_result expected) {
    web_socket_origin origin(opcode, std::move(payload),
        expected == expected_result::authenticated ||
            expected == expected_result::authentication_rejected ||
            expected == expected_result::delivered || expected == expected_result::delivery_rejected);
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    ruvia::websocket_client client(
        loop, {
                  .scheme_ = ruvia::websocket_scheme::ws,
                  .host_ = "127.0.0.1",
                  .port_ = origin.port(),
                  .target_ = "/api/agent/connect",
                  .subprotocols_ = {std::string(flexedge::node::control_subprotocol)},
              });
    loops.start();
    auto task = loop.start(exercise(client, expected));
    const auto result = task.get();
    loops.stop();
    loops.join();
    return result;
}

}  // namespace

void verify_delivery_protocol() {
    using namespace flexedge::node;
    const auto require = [](bool value) {
        if (!value) {
            throw std::runtime_error("delivery protocol regression");
        }
    };
    v2::DesiredState state;
    state.mutable_node_spec()->mutable_content()->set_revision(8);
    state.mutable_release()->mutable_content()->set_release_id("new-release");
    state.mutable_release()->set_digest_sha256("new-digest");
    const auto changed = release_probe_replies("watch-1", "binary", 7, "older", "older", state);
    require(changed.size() == 2);
    require(changed[0].has_release_probe_ack() && changed[1].has_desired_state());
    require(changed[0].request_id() == changed[1].request_id());
    require(changed[0].release_probe_ack().desired_release_id() ==
            changed[1].desired_state().release().content().release_id());
    require(changed[0].release_probe_ack().desired_node_spec_revision() == 8);
    require(release_probe_replies("watch-2", "binary", 8, "new-release", "new-digest", {}).size() ==
            1);

    const std::string digest(64, 'a');
    v2::Welcome welcome;
    require(!valid_welcome(welcome));
    welcome.set_node_id("node-1");
    welcome.set_desired_node_spec_revision(1);
    welcome.set_desired_release_id("release-1");
    welcome.set_desired_manifest_digest(digest);
    welcome.set_node_binary_sha256(digest);
    require(valid_welcome(welcome));
    for (int field = 0; field < 5; ++field) {
        auto invalid = welcome;
        switch (field) {
            case 0:
                invalid.clear_node_id();
                break;
            case 1:
                invalid.set_desired_node_spec_revision(0);
                break;
            case 2:
                invalid.clear_desired_release_id();
                break;
            case 3:
                invalid.set_desired_manifest_digest("bad");
                break;
            case 4:
                invalid.set_node_binary_sha256("bad");
                break;
        }
        require(!valid_welcome(invalid));
    }
    auto probe = release_probe_replies("probe-1", digest, 1, "release-1", digest, {}).front();
    require(matches_release_probe_ack(probe, "probe-1"));
    require(!matches_release_probe_ack(probe, "wrong-id"));
    require(!matches_heartbeat_ack(probe, "probe-1"));
    probe.mutable_release_probe_ack()->set_desired_manifest_digest("invalid");
    require(!matches_release_probe_ack(probe, "probe-1"));
    v2::ServerEnvelope heartbeat_reply;
    heartbeat_reply.set_request_id("heartbeat-1");
    heartbeat_reply.mutable_heartbeat_ack()->set_node_binary_sha256(digest);
    require(matches_heartbeat_ack(heartbeat_reply, "heartbeat-1"));
    require(!matches_heartbeat_ack(heartbeat_reply, "wrong-id"));
    require(!matches_release_probe_ack(heartbeat_reply, "heartbeat-1"));
    heartbeat_reply.mutable_heartbeat_ack()->set_node_binary_sha256("invalid");
    require(!matches_heartbeat_ack(heartbeat_reply, "heartbeat-1"));

    v2::ApplyResult result;
    result.set_node_id("node-1");
    result.set_node_spec_revision(8);
    result.set_release_id("new-release");
    result.set_manifest_digest("new-digest");
    result.set_applied(true);
    v2::ServerEnvelope reply;
    reply.set_request_id("watch-1");
    auto* ack = reply.mutable_apply_result_ack();
    ack->set_node_id("node-1");
    ack->set_node_spec_revision(8);
    ack->set_release_id("new-release");
    ack->set_manifest_digest("new-digest");
    ack->set_applied(true);
    require(matches_apply_result_ack(reply, "watch-1", result));
    require(!matches_apply_result_ack(reply, "watch-2", result));
    ack->set_release_id("old-release");
    require(!matches_apply_result_ack(reply, "watch-1", result));
    ack->set_release_id("new-release");
    ack->set_node_id("other-node");
    require(!matches_apply_result_ack(reply, "watch-1", result));
    ack->set_node_id("node-1");
    ack->set_node_spec_revision(7);
    require(!matches_apply_result_ack(reply, "watch-1", result));
    ack->set_node_spec_revision(8);
    ack->set_manifest_digest("wrong");
    require(!matches_apply_result_ack(reply, "watch-1", result));
    ack->set_manifest_digest("new-digest");
    ack->set_applied(false);
    require(!matches_apply_result_ack(reply, "watch-1", result));
    result.set_applied(false);
    require(matches_apply_result_ack(reply, "watch-1", result));
}

ruvia::task<bool> watch_signal(service::node_dispatch::fanout::hub::subscription& subscription,
    std::chrono::milliseconds timeout) {
    co_return (co_await subscription.receive_for(timeout, {})).has_value();
}

// One Web Worker's delivery state, as its reader sees it.
struct worker_states final {
    service::live_resource::hub live_;
    service::live_resource::node_deadlines deadlines_;
    service::node_dispatch::fanout::hub dispatch_;

    template <typename state_type>
    state_type& worker_state() {
        if constexpr (std::is_same_v<state_type, service::live_resource::hub>) {
            return live_;
        } else if constexpr (std::is_same_v<state_type, service::live_resource::node_deadlines>) {
            return deadlines_;
        } else {
            return dispatch_;
        }
    }
};

void verify_watch_notifications() {
    using service::live_resource::notifications::resource_notification;
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    worker_states states;
    auto watch = states.dispatch_.subscribe(loop.handle(), "delivery-test");
    loops.start();
    const auto deliver = [&](resource_notification value) {
        service::live_resource::deliver_notification(states, value, std::nullopt);
    };
    deliver(std::move(resource_notification("delivery-test").node_runtime("node-runtime-test", "{}")));
    if (loop.start(watch_signal(watch, std::chrono::milliseconds(20))).get()) {
        throw std::runtime_error("heartbeats must not wake configuration watches");
    }
    deliver(std::move(resource_notification("other-tenant").node_config()));
    if (loop.start(watch_signal(watch, std::chrono::milliseconds(20))).get()) {
        throw std::runtime_error("cross-tenant wakeup");
    }
    // Covers commit after subscribe but before awaiting the channel.
    deliver(std::move(resource_notification("delivery-test").node_config()));
    if (!loop.start(watch_signal(watch, std::chrono::seconds(1))).get()) {
        throw std::runtime_error("queued commit lost");
    }
    auto pending = loop.start(watch_signal(watch, std::chrono::seconds(1)));
    deliver(std::move(resource_notification("delivery-test").node_config()));
    if (!pending.get()) {
        throw std::runtime_error("waiting watch not woken");
    }
    // A missed notification of the tenant re-reads every watch once.
    resource_notification missed("delivery-test");
    missed.missed_until_ = 1;
    deliver(std::move(missed));
    if (!loop.start(watch_signal(watch, std::chrono::seconds(1))).get()) {
        throw std::runtime_error("recovery did not wake the watch");
    }
    loops.stop();
    loops.join();
}

void verify_heartbeat_schedule() {
    using flexedge::node::release_watch_seconds;
    const auto require = [](bool value) {
        if (!value) {
            throw std::runtime_error("heartbeat schedule regression");
        }
    };
    // The watch lasts until the heartbeat is due, rounded up, so a 5 second interval stays
    // 5 seconds instead of 4.
    require(release_watch_seconds(std::chrono::milliseconds(4200)) == 5);
    require(release_watch_seconds(std::chrono::milliseconds(4999)) == 5);
    require(release_watch_seconds(std::chrono::seconds(5)) == 5);
    require(release_watch_seconds(std::chrono::milliseconds(1)) == 1);
    require(release_watch_seconds(std::chrono::steady_clock::duration::zero()) == 0);
    require(release_watch_seconds(-std::chrono::seconds(1)) == 0);
    require(release_watch_seconds(std::chrono::milliseconds(29999)) ==
            flexedge::node::release_watch_maximum_seconds);

    // The connected IP count is optional: absent (older nodes, or not counted) differs from 0.
    flexedge::node::v2::Heartbeat heartbeat;
    heartbeat.set_connection_count(3);
    flexedge::node::v2::Heartbeat decoded;
    require(decoded.ParseFromString(heartbeat.SerializeAsString()) &&
            !decoded.has_connected_ip_count() && decoded.connection_count() == 3);
    heartbeat.set_connected_ip_count(0);
    require(decoded.ParseFromString(heartbeat.SerializeAsString()) &&
            decoded.has_connected_ip_count() && decoded.connected_ip_count() == 0);
    heartbeat.set_connected_ip_count(2);
    require(decoded.ParseFromString(heartbeat.SerializeAsString()) &&
            decoded.connected_ip_count() == 2);
}

int main() {
    flexedge::node::v2::ServerEnvelope delivery_reply;
    delivery_reply.set_request_id("logs-1");
    delivery_reply.mutable_log_delivery_ack();
    if (run_case(0x2, delivery_reply.SerializeAsString(), expected_result::delivered)) {
        return 14;
    }
    delivery_reply.set_request_id("wrong");
    if (run_case(0x2, delivery_reply.SerializeAsString(), expected_result::delivery_rejected)) {
        return 15;
    }
    delivery_reply.set_request_id("logs-1");
    delivery_reply.mutable_heartbeat_ack();
    if (run_case(0x2, delivery_reply.SerializeAsString(), expected_result::delivery_rejected)) {
        return 16;
    }
    flexedge::node::v2::ServerEnvelope authentication_reply;
    authentication_reply.set_request_id("auth");
    authentication_reply.mutable_welcome()->set_node_id("node");
    if (run_case(0x2, authentication_reply.SerializeAsString(), expected_result::authenticated)) {
        return 11;
    }
    authentication_reply.set_request_id("other");
    if (run_case(0x2, authentication_reply.SerializeAsString(), expected_result::authentication_rejected)) {
        return 12;
    }
    authentication_reply.set_request_id("auth");
    authentication_reply.mutable_heartbeat_ack();
    if (run_case(0x2, authentication_reply.SerializeAsString(), expected_result::authentication_rejected)) {
        return 13;
    }
    try {
        const auto defaults = flexedge::node::control_transport_config("edge.example");
        const auto explicit_address =
            flexedge::node::control_transport_config("ws://[::1]:8080/custom");
        if (defaults.scheme_ != ruvia::websocket_scheme::wss ||
            defaults.host_ != "edge.example" || defaults.port_.has_value() ||
            defaults.target_ != "/api/agent/connect" ||
            defaults.subprotocols_ != std::vector<std::string>{
                                          std::string(flexedge::node::control_subprotocol)} ||
            explicit_address.scheme_ != ruvia::websocket_scheme::ws || explicit_address.host_ != "::1" || explicit_address.port_ != 8080 || explicit_address.target_ != "/custom") {
            throw std::runtime_error("control transport address mapping failed");
        }
        for (const auto address : {"", "ws://", "edge.example:0", "edge.example:65536",
                 "edge.example:12x", "ws://[::1", "user@edge.example",
                 "edge.example/path?query", "edge.example/#fragment",
                 "http://edge.example", "edge.example:", "ws://[::1]:",
                 "::1", "edge.example\n", "edge.example/path with space",
                 "edge.example\\path", "edge.example]", "ws://[[::1]",
                 "ws://[example.com]", "ws://[127.0.0.1]", "ws://[1::2::3]"}) {
            bool rejected = false;
            try {
                (void)flexedge::node::control_transport_config(address);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            if (!rejected) {
                throw std::runtime_error("invalid control address accepted");
            }
        }
        verify_delivery_protocol();
        verify_watch_notifications();
        verify_heartbeat_schedule();
        if constexpr (flexedge::node::release_watch_maximum_seconds != 10) {
            return 8;
        }
        flexedge::node::v2::ServerEnvelope envelope;
        envelope.set_request_id("heartbeat-1");
        envelope.mutable_heartbeat_ack()->set_node_binary_sha256(
            "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea");
        std::string bytes;
        if (!envelope.SerializeToString(&bytes)) {
            return 1;
        }
        if (run_case(0x2, std::move(bytes), expected_result::envelope) != 0) {
            return 2;
        }
        if (run_case(0x1, "text", expected_result::protocol_error) != 0) {
            return 3;
        }
        if (run_case(0x8, std::string{"\x03\xe8", 2}, expected_result::stream_ended) != 0) {
            return 4;
        }
        flexedge::node::v2::ClientEnvelope release_probe;
        release_probe.set_request_id("release-probe-1");
        release_probe.mutable_release_probe();
        std::string release_probe_bytes;
        if (!release_probe.SerializeToString(&release_probe_bytes)) {
            return 5;
        }
        flexedge::node::v2::ClientEnvelope decoded_release_probe;
        if (!decoded_release_probe.ParseFromString(release_probe_bytes) ||
            decoded_release_probe.request_id() != "release-probe-1" ||
            !decoded_release_probe.has_release_probe()) {
            return 6;
        }
        flexedge::node::v2::ServerEnvelope release_probe_ack;
        release_probe_ack.set_request_id("release-probe-1");
        release_probe_ack.mutable_release_probe_ack()->set_node_binary_sha256(
            "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea");
        if (!release_probe_ack.has_release_probe_ack() ||
            release_probe_ack.release_probe_ack().node_binary_sha256() !=
                "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea") {
            return 7;
        }
        flexedge::node::v2::ClientEnvelope log_authenticate;
        log_authenticate.set_request_id("log-authenticate");
        log_authenticate.mutable_authenticate()->set_session_purpose(
            flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST);
        std::string log_authenticate_bytes;
        if (!log_authenticate.SerializeToString(&log_authenticate_bytes)) {
            return 9;
        }
        flexedge::node::v2::ClientEnvelope decoded_log_authenticate;
        if (!decoded_log_authenticate.ParseFromString(log_authenticate_bytes) ||
            !decoded_log_authenticate.has_authenticate() ||
            decoded_log_authenticate.authenticate().session_purpose() !=
                flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST) {
            return 10;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 8;
    }
}
