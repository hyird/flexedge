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
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "node/control/authenticate.h"
#include "node/proto/control_protocol.h"

namespace {

enum class expected_result : std::uint8_t {
    envelope,
    stream_ended,
    protocol_error,
    authenticated,
    authentication_rejected,
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
            expected == expected_result::authentication_rejected);
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

int main() {
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 8;
    }
}
