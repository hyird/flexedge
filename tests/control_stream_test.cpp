#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/http/detail/field/HeaderTokenUtils.h>
#include <ruvia/http/detail/util/AsciiCase.h>
#include <ruvia/http/detail/websocket/handshake/HttpWebSocketAcceptKey.h>
#include <ruvia/web/WebSocketClient.h>

#include "node/control/control_stream.h"
#include "node/control/transport_config.h"
#include "node/control/authenticate.h"
#include "node/control/log_delivery.h"
#include "node/proto/control_reply.h"
#include "service/features/node_dispatch/notifications.h"
#include "service/features/live_resource/fanout.h"
#include "node/proto/control_protocol.h"

namespace {

enum class ExpectedResult : std::uint8_t {
    kEnvelope,
    kStreamEnded,
    kProtocolError,
    kAuthenticated,
    kAuthenticationRejected,
    kDelivered,
    kDeliveryRejected,
};

class WebSocketOrigin final {
  public:
    WebSocketOrigin(std::uint8_t opcode, std::string payload, bool authentication = false)
        : opcode_(opcode), payload_(std::move(payload)), authentication_(authentication),
          acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          thread_([this] { serve(); }) {}

    ~WebSocketOrigin() {
        std::error_code ignored;
        acceptor_.close(ignored);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

  private:
    static std::string header(std::string_view request, std::string_view name) {
        std::size_t cursor = request.find("\r\n") + 2;
        while (cursor < request.size()) {
            const auto end = request.find("\r\n", cursor);
            if (end == cursor || end == std::string_view::npos) {
                break;
            }
            const auto line = request.substr(cursor, end - cursor);
            const auto colon = line.find(':');
            if (colon != std::string_view::npos &&
                ruvia::detail::httpAsciiEqualsIgnoreCase(line.substr(0, colon), name)) {
                return std::string(ruvia::detail::httpTrimOws(line.substr(colon + 1)));
            }
            cursor = end + 2;
        }
        return {};
    }

    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            asio::streambuf buffer;
            asio::read_until(socket, buffer, "\r\n\r\n");
            const std::string request(asio::buffers_begin(buffer.data()),
                                      asio::buffers_end(buffer.data()));
            const auto key = header(request, "Sec-WebSocket-Key");
            if (key.empty() ||
                !ruvia::detail::httpHasToken(header(request, "Upgrade"), "websocket")) {
                return;
            }
            ruvia::detail::WebSocketAcceptKey accept{};
            ruvia::detail::encodeWebSocketAccept(accept, key);
            std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                                   "Connection: Upgrade\r\nSec-WebSocket-Accept: ";
            response.append(accept.data(), accept.size());
            response.append("\r\nSec-WebSocket-Protocol: ");
            response.append(flexedge::node::kControlSubprotocol);
            response.append("\r\n\r\n");
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

ruvia::Task<int> exercise(ruvia::WebSocketClient& client, ExpectedResult expected) {
    co_await client.connect();
    const auto connection = client.withOptions({});
    try {
        if (expected == ExpectedResult::kDelivered || expected == ExpectedResult::kDeliveryRejected) {
            flexedge::node::NodeLogBuffer logs;
            (void)logs.node("info", "test", "retained until acknowledged");
            try {
                co_await flexedge::node::deliverLogs(connection, logs, "node", 1);
                co_return expected == ExpectedResult::kDelivered &&
                    logs.queuedEvents() == 0 && logs.retainedEvents() == 0 ? 0 : 6;
            } catch (const std::runtime_error& error) {
                const auto retry = logs.take("node");
                co_return expected == ExpectedResult::kDeliveryRejected &&
                    std::string_view(error.what()) == "control plane did not acknowledge log delivery" &&
                    logs.queuedEvents() == 1 && logs.retainedEvents() == 1 && retry.has_value() ? 0 : 7;
            }
        }
        if (expected == ExpectedResult::kAuthenticated ||
            expected == ExpectedResult::kAuthenticationRejected) {
            flexedge::node::v2::ClientEnvelope authentication;
            authentication.set_request_id("auth");
            authentication.mutable_authenticate()->set_node_id("node");
            authentication.mutable_authenticate()->set_secret("synthetic-secret");
            try {
                const auto welcome = co_await flexedge::node::authenticateControlEnvelope(
                    connection, std::move(authentication), "authentication rejected");
                co_return expected == ExpectedResult::kAuthenticated &&
                    welcome.node_id() == "node" ? 0 : 4;
            } catch (const std::runtime_error& error) {
                co_return expected == ExpectedResult::kAuthenticationRejected &&
                    std::string_view(error.what()) == "authentication rejected" ? 0 : 5;
            }
        }
        const auto envelope = co_await flexedge::node::readServerEnvelope(connection);
        const bool matched = expected == ExpectedResult::kEnvelope &&
                             envelope.has_heartbeat_ack() &&
                             envelope.heartbeat_ack().node_binary_sha256() ==
                                 "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea";
        co_return matched ? 0 : 1;
    } catch (const flexedge::node::ControlStreamEnded&) {
        co_return expected == ExpectedResult::kStreamEnded ? 0 : 2;
    } catch (const std::runtime_error& error) {
        const bool matched =
            expected == ExpectedResult::kProtocolError &&
            std::string_view(error.what()) == "control plane must send protobuf binary messages";
        co_return matched ? 0 : 3;
    }
}

int runCase(std::uint8_t opcode, std::string payload, ExpectedResult expected) {
    WebSocketOrigin origin(opcode, std::move(payload),
        expected == ExpectedResult::kAuthenticated ||
        expected == ExpectedResult::kAuthenticationRejected ||
        expected == ExpectedResult::kDelivered || expected == ExpectedResult::kDeliveryRejected);
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    ruvia::WebSocketClient client(
        loop, {
                  .scheme = ruvia::WebSocketScheme::kWs,
                  .host = "127.0.0.1",
                  .port = origin.port(),
                  .target = "/api/agent/connect",
                  .subprotocols = {std::string(flexedge::node::kControlSubprotocol)},
              });
    loops.start();
    auto task = loop.start(exercise(client, expected));
    const auto result = task.get();
    loops.stop();
    loops.join();
    return result;
}

} // namespace

void verifyDeliveryProtocol() {
    using namespace flexedge::node;
    const auto require = [](bool value) {
        if (!value)
            throw std::runtime_error("delivery protocol regression");
    };
    v2::DesiredState state;
    state.mutable_node_spec()->mutable_content()->set_revision(8);
    state.mutable_release()->mutable_content()->set_release_id("new-release");
    state.mutable_release()->set_digest_sha256("new-digest");
    const auto changed = releaseProbeReplies("watch-1", "binary", 7, "older", "older", state);
    require(changed.size() == 2);
    require(changed[0].has_release_probe_ack() && changed[1].has_desired_state());
    require(changed[0].request_id() == changed[1].request_id());
    require(changed[0].release_probe_ack().desired_release_id() ==
            changed[1].desired_state().release().content().release_id());
    require(changed[0].release_probe_ack().desired_node_spec_revision() == 8);
    require(releaseProbeReplies("watch-2", "binary", 8, "new-release", "new-digest", {}).size() ==
            1);

    const std::string digest(64, 'a');
    v2::Welcome welcome;
    require(!validWelcome(welcome));
    welcome.set_node_id("node-1");
    welcome.set_desired_node_spec_revision(1);
    welcome.set_desired_release_id("release-1");
    welcome.set_desired_manifest_digest(digest);
    welcome.set_node_binary_sha256(digest);
    require(validWelcome(welcome));
    for (int field = 0; field < 5; ++field) {
        auto invalid = welcome;
        switch (field) {
            case 0: invalid.clear_node_id(); break;
            case 1: invalid.set_desired_node_spec_revision(0); break;
            case 2: invalid.clear_desired_release_id(); break;
            case 3: invalid.set_desired_manifest_digest("bad"); break;
            case 4: invalid.set_node_binary_sha256("bad"); break;
        }
        require(!validWelcome(invalid));
    }
    auto probe = releaseProbeReplies("probe-1", digest, 1, "release-1", digest, {}).front();
    require(matchesReleaseProbeAck(probe, "probe-1"));
    require(!matchesReleaseProbeAck(probe, "wrong-id"));
    require(!matchesHeartbeatAck(probe, "probe-1"));
    probe.mutable_release_probe_ack()->set_desired_manifest_digest("invalid");
    require(!matchesReleaseProbeAck(probe, "probe-1"));
    v2::ServerEnvelope heartbeatReply;
    heartbeatReply.set_request_id("heartbeat-1");
    heartbeatReply.mutable_heartbeat_ack()->set_node_binary_sha256(digest);
    require(matchesHeartbeatAck(heartbeatReply, "heartbeat-1"));
    require(!matchesHeartbeatAck(heartbeatReply, "wrong-id"));
    require(!matchesReleaseProbeAck(heartbeatReply, "heartbeat-1"));
    heartbeatReply.mutable_heartbeat_ack()->set_node_binary_sha256("invalid");
    require(!matchesHeartbeatAck(heartbeatReply, "heartbeat-1"));

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
    require(matchesApplyResultAck(reply, "watch-1", result));
    require(!matchesApplyResultAck(reply, "watch-2", result));
    ack->set_release_id("old-release");
    require(!matchesApplyResultAck(reply, "watch-1", result));
    ack->set_release_id("new-release");
    ack->set_node_id("other-node");
    require(!matchesApplyResultAck(reply, "watch-1", result));
    ack->set_node_id("node-1");
    ack->set_node_spec_revision(7);
    require(!matchesApplyResultAck(reply, "watch-1", result));
    ack->set_node_spec_revision(8);
    ack->set_manifest_digest("wrong");
    require(!matchesApplyResultAck(reply, "watch-1", result));
    ack->set_manifest_digest("new-digest");
    ack->set_applied(false);
    require(!matchesApplyResultAck(reply, "watch-1", result));
    result.set_applied(false);
    require(matchesApplyResultAck(reply, "watch-1", result));
}

ruvia::Task<bool> watchSignal(service::node_dispatch::fanout::Hub::Subscription& subscription,
                              std::chrono::milliseconds timeout) {
    co_return (co_await subscription.receiveFor(timeout, {})).hasValue();
}

void verifyWatchNotifications() {
    ruvia::EventLoopPool loops({.loopCount = 1});
    const auto loop = loops.loop(0);
    auto watch =
        service::node_dispatch::notifications::hub().subscribe(loop.handle(), "delivery-test");
    loops.start();
    service::live_resource::hub().publishRuntime("delivery-test", "node-runtime-test", "{}");
    if (loop.start(watchSignal(watch, std::chrono::milliseconds(20))).get())
        throw std::runtime_error("heartbeats must not wake configuration watches");
    service::node_dispatch::notifications::published("other-tenant");
    if (loop.start(watchSignal(watch, std::chrono::milliseconds(20))).get())
        throw std::runtime_error("cross-tenant wakeup");
    // Covers commit after subscribe but before awaiting the channel.
    service::node_dispatch::notifications::published("delivery-test");
    if (!loop.start(watchSignal(watch, std::chrono::seconds(1))).get())
        throw std::runtime_error("queued commit lost");
    auto pending = loop.start(watchSignal(watch, std::chrono::seconds(1)));
    service::node_dispatch::notifications::published("delivery-test");
    if (!pending.get())
        throw std::runtime_error("waiting watch not woken");
    loops.stop();
    loops.join();
}

int main() {
    flexedge::node::v2::ServerEnvelope deliveryReply;
    deliveryReply.set_request_id("logs-1");
    deliveryReply.mutable_log_delivery_ack();
    if (runCase(0x2, deliveryReply.SerializeAsString(), ExpectedResult::kDelivered)) return 14;
    deliveryReply.set_request_id("wrong");
    if (runCase(0x2, deliveryReply.SerializeAsString(), ExpectedResult::kDeliveryRejected)) return 15;
    deliveryReply.set_request_id("logs-1");
    deliveryReply.mutable_heartbeat_ack();
    if (runCase(0x2, deliveryReply.SerializeAsString(), ExpectedResult::kDeliveryRejected)) return 16;
    flexedge::node::v2::ServerEnvelope authenticationReply;
    authenticationReply.set_request_id("auth");
    authenticationReply.mutable_welcome()->set_node_id("node");
    if (runCase(0x2, authenticationReply.SerializeAsString(), ExpectedResult::kAuthenticated))
        return 11;
    authenticationReply.set_request_id("other");
    if (runCase(0x2, authenticationReply.SerializeAsString(), ExpectedResult::kAuthenticationRejected))
        return 12;
    authenticationReply.set_request_id("auth");
    authenticationReply.mutable_heartbeat_ack();
    if (runCase(0x2, authenticationReply.SerializeAsString(), ExpectedResult::kAuthenticationRejected))
        return 13;
    try {
        const auto defaults = flexedge::node::controlTransportConfig("edge.example");
        const auto explicitAddress =
            flexedge::node::controlTransportConfig("ws://[::1]:8080/custom");
        if (defaults.scheme != ruvia::WebSocketScheme::kWss ||
            defaults.host != "edge.example" || defaults.port.has_value() ||
            defaults.target != "/api/agent/connect" ||
            defaults.subprotocols != std::vector<std::string>{
                std::string(flexedge::node::kControlSubprotocol)} ||
            explicitAddress.scheme != ruvia::WebSocketScheme::kWs ||
            explicitAddress.host != "::1" || explicitAddress.port != 8080 ||
            explicitAddress.target != "/custom") {
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
                (void)flexedge::node::controlTransportConfig(address);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            if (!rejected) throw std::runtime_error("invalid control address accepted");
        }
        verifyDeliveryProtocol();
        verifyWatchNotifications();
        if constexpr (flexedge::node::kReleaseWatchMaximumSeconds != 10) {
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
        if (runCase(0x2, std::move(bytes), ExpectedResult::kEnvelope) != 0) {
            return 2;
        }
        if (runCase(0x1, "text", ExpectedResult::kProtocolError) != 0) {
            return 3;
        }
        if (runCase(0x8, std::string{"\x03\xe8", 2}, ExpectedResult::kStreamEnded) != 0) {
            return 4;
        }
        flexedge::node::v2::ClientEnvelope releaseProbe;
        releaseProbe.set_request_id("release-probe-1");
        releaseProbe.mutable_release_probe();
        std::string releaseProbeBytes;
        if (!releaseProbe.SerializeToString(&releaseProbeBytes)) {
            return 5;
        }
        flexedge::node::v2::ClientEnvelope decodedReleaseProbe;
        if (!decodedReleaseProbe.ParseFromString(releaseProbeBytes) ||
            decodedReleaseProbe.request_id() != "release-probe-1" ||
            !decodedReleaseProbe.has_release_probe()) {
            return 6;
        }
        flexedge::node::v2::ServerEnvelope releaseProbeAck;
        releaseProbeAck.set_request_id("release-probe-1");
        releaseProbeAck.mutable_release_probe_ack()->set_node_binary_sha256(
            "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea");
        if (!releaseProbeAck.has_release_probe_ack() ||
            releaseProbeAck.release_probe_ack().node_binary_sha256() !=
                "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea") {
            return 7;
        }
        flexedge::node::v2::ClientEnvelope logAuthenticate;
        logAuthenticate.set_request_id("log-authenticate");
        logAuthenticate.mutable_authenticate()->set_session_purpose(
            flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST);
        std::string logAuthenticateBytes;
        if (!logAuthenticate.SerializeToString(&logAuthenticateBytes)) {
            return 9;
        }
        flexedge::node::v2::ClientEnvelope decodedLogAuthenticate;
        if (!decodedLogAuthenticate.ParseFromString(logAuthenticateBytes) ||
            !decodedLogAuthenticate.has_authenticate() ||
            decodedLogAuthenticate.authenticate().session_purpose() !=
                flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST) {
            return 10;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 8;
    }
}
