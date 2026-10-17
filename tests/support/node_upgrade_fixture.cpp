// Isolated integration helper. See node_upgrade_transition.py for the CLI contract.
#include <ruvia/http/http2_connection.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "node/runtime/state_store.h"
#include "node/runtime/version.h"
#include "tests/support/config_fixture.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
std::uint16_t port(const char* value) {
    const auto parsed = std::stoul(value);
    require(parsed > 0 && parsed <= 65535, "invalid fixture port");
    return static_cast<std::uint16_t>(parsed);
}
void write_state(const std::filesystem::path& root, std::uint16_t http_port,
    std::uint16_t https_port, std::uint16_t origin_port) {
    auto config = flexedge::testing::snapshot(false);
    const std::string node_id(32, 'a');
    const std::string secret(32, 'b');
    config.node_spec.set_node_id(node_id);
    auto* endpoint = config.node_spec.mutable_endpoints(0);
    endpoint->set_http_port(http_port);
    endpoint->set_https_port(https_port);
    auto* website = config.mutableWebsite();
    website->set_https_enabled(false);
    website->set_force_https(false);
    website->set_health_check_enabled(false);
    website->set_access_log_enabled(false);
    website->set_origin_http_version("http1");
    website->set_response_compression_enabled(false);
    website->set_origin_read_timeout_seconds(45);
    website->clear_domains();
    auto* domain = website->add_domains();
    domain->set_hostname("upgrade.invalid");
    domain->set_https_enabled(false);
    auto* origin = website->mutable_origins(0);
    origin->set_host("127.0.0.1");
    origin->set_port(origin_port);
    config.finalize();
    flexedge::node::state_store store(root / "state", secret);
    store.stage(config.active, config.objects);
    (void)store.activate_staged();
    flexedge::node::write_secure_file_atomic(root / "fixture-active.pb", config.active.SerializeAsString());
    flexedge::node::write_secure_file_atomic(root / "credentials",
        "node_id=" + node_id + "\nsecret=" + secret + "\n");
    std::cout << "{\"node_id\":\"" << node_id << "\",\"secret\":\"" << secret
              << "\",\"revision\":" << config.node_spec.revision()
              << ",\"release_id\":\"" << config.release.release_id()
              << "\",\"manifest_digest\":\"" << config.active.release().digest_sha256()
              << "\",\"version\":\"" << flexedge::node::node_version << "\"}\n";
}
void h2_client(std::uint16_t http_port) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), http_port});
    socket.non_blocking(true);
    auto client = ruvia::http2_connection::client();
    const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
        .method_ = "GET", .authority_ = "upgrade.invalid", .target_ = "/slow-h2", .content_ = ruvia::http2_request_content::none()});
    require(submitted.submitted() != nullptr, "H2 request submission failed");
    const auto id = submitted.submitted()->stream_id();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(50);
    std::pmr::string output;
    std::size_t sent{};
    const auto flush = [&] {
        while (std::chrono::steady_clock::now() < deadline) {
            if (sent == output.size()) {
                output.clear();
                sent = 0;
                if (!client.wants_write()) {
                    return;
                }
                client.take_output(output);
            }
            std::error_code error;
            sent += socket.write_some(asio::buffer(output.data() + sent, output.size() - sent), error);
            require(!error || error == asio::error::would_block || error == asio::error::try_again,
                "H2 socket write failed");
            if (error) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        throw std::runtime_error("H2 write deadline expired");
    };
    flush();
    std::array<char, 16384> input{};
    std::size_t received{};
    bool first{}, goaway{}, ended{};
    unsigned status{};
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        const auto size = socket.read_some(asio::buffer(input), error);
        require(!error || error == asio::error::would_block || error == asio::error::try_again,
            "H2 closed before GOAWAY and complete response");
        if (size != 0) {
            require(client.feed(std::string_view(input.data(), size)) != ruvia::http2_feed_result::protocol_failure,
                "H2 protocol failure");
            while (auto event = client.next_event()) {
                if (const auto* head = event->response_head()) {
                    require(head->stream_id() == id, "unexpected H2 response stream");
                    status = head->head().status().value();
                } else if (const auto* body = event->message_body_chunk()) {
                    require(body->stream_id() == id, "unexpected H2 body stream");
                    require(std::ranges::all_of(body->bytes(), [](char c) { return c == 'x'; }),
                        "corrupted H2 response body");
                    received += body->bytes().size();
                    if (!first && received >= 4096) {
                        first = true;
                        std::cout << "{\"event\":\"first\",\"bytes\":" << received << "}" << std::endl;
                    }
                } else if (const auto* end = event->message_end()) {
                    require(end->stream_id() == id, "unexpected H2 end stream");
                    ended = true;
                } else if (const auto* away = event->goaway()) {
                    require(away->error() == ruvia::http2_error_code::no_error,
                        "H2 GOAWAY did not use NO_ERROR");
                    require(away->last_stream_id() >= id, "H2 GOAWAY rejected existing stream");
                    goaway = true;
                    std::cout << "{\"event\":\"goaway\",\"error\":0,\"last_stream_id\":"
                              << away->last_stream_id() << "}" << std::endl;
                    // GOAWAY retires new requests, not this already accepted response.
                } else if (event->stream_closed() || event->request_unprocessed()) {
                    throw std::runtime_error("H2 stream reset or rejected during drain");
                }
            }
            flush();
        }
        if (ended && goaway) {
            require(first && status == 200 && received == 65536, "incomplete H2 response");
            std::cout << "{\"event\":\"complete\",\"bytes\":" << received << "}" << std::endl;
            return;
        }
        if (error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    throw std::runtime_error("H2 response/GOAWAY deadline expired");
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 6 && std::string_view(argv[1]) == "state") {
            write_state(argv[2], port(argv[3]), port(argv[4]), port(argv[5]));
        } else if (argc == 3 && std::string_view(argv[1]) == "h2") {
            h2_client(port(argv[2]));
        } else {
            throw std::runtime_error("usage: node_upgrade_fixture state ROOT HTTP_PORT HTTPS_PORT ORIGIN_PORT | h2 HTTP_PORT");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
