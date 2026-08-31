#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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
#include "node/proto/control_protocol.h"

namespace {

enum class ExpectedResult : std::uint8_t {
    kEnvelope,
    kStreamEnded,
    kProtocolError,
};

class WebSocketOrigin final {
  public:
    WebSocketOrigin(std::uint8_t opcode, std::string payload)
        : opcode_(opcode), payload_(std::move(payload)),
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
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::thread thread_;
};

ruvia::Task<int> exercise(ruvia::WebSocketClient& client, ExpectedResult expected) {
    co_await client.connect();
    const auto connection = client.withOptions({});
    try {
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
    WebSocketOrigin origin(opcode, std::move(payload));
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

int main() {
    try {
        if constexpr (flexedge::node::kReleaseProbeIntervalSeconds != 1) {
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
