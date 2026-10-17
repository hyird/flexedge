#include <atomic>
#include <iostream>
#include <thread>

#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/cache_directory.h"
#include "tests/support/config_fixture.h"
#include "tests/support/tls_identity.h"

using namespace flexedge::node;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
class origin final {
public:
    origin()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}) {
        accept();
        thread_ = std::jthread([this] { io_.run(); });
    }
    ~origin() {
        asio::post(io_, [this] { acceptor_.close(); io_.stop(); });
        thread_.join();
    }
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    void finish() {
        release_ = true;
    }
    void reset() {
        release_ = false;
    }

private:
    void finish_later(std::shared_ptr<asio::ip::tcp::socket> socket) {
        auto timer = std::make_shared<asio::steady_timer>(io_);
        timer->expires_after(std::chrono::milliseconds(5));
        timer->async_wait([this, socket, timer](std::error_code error) {
            if (error) {
                return;
            }
            if (release_) {
                std::error_code ignored;
                socket->close(ignored);
            } else {
                finish_later(socket);
            }
        });
    }
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket peer) {
            if (error) {
                return;
            }
            auto socket = std::make_shared<asio::ip::tcp::socket>(std::move(peer));
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*socket, *request, "\r\n\r\n", [this, socket, request](std::error_code read_error, std::size_t) {
                if (read_error) {
                    return;
                }
                std::string input(asio::buffers_begin(request->data()), asio::buffers_end(request->data()));
                const bool websocket = input.starts_with("GET /ws ");
                auto output = std::make_shared<std::string>(websocket
                                                                ? "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n"
                                                                : "HTTP/1.1 103 Early Hints\r\nLink: </app.css>; rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nConnection: close\r\n\r\ndata: hello\n\n");
                asio::async_write(*socket, asio::buffer(*output), [this, socket, output, websocket](std::error_code write_error, std::size_t) {
                    if (write_error) {
                        return;
                    }
                    if (!websocket) {
                        finish_later(socket);
                        return;
                    }
                    auto buffer = std::make_shared<std::array<char, 64>>();
                    socket->async_read_some(asio::buffer(*buffer), [this, socket, buffer](std::error_code error, std::size_t size) {
                        if (error) {
                            return;
                        }
                        asio::async_write(*socket, asio::buffer(buffer->data(), size), [this, socket, buffer](std::error_code, std::size_t) { finish_later(socket); });
                    });
                });
            });
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> release_{};
    std::jthread thread_;
};
ruvia::task<void> activate(data_plane& plane, std::shared_ptr<const compiled_config> config) {
    auto prepared = co_await config_activation::prepare(plane, std::move(config));
    co_await prepared.commit();
}
ruvia::task<std::vector<v2::AccessLog>> collect(ruvia::channel_receiver<owned_log_batch>& receiver, std::chrono::milliseconds wait) {
    std::vector<v2::AccessLog> logs;
    for (;;) {
        auto batch = co_await receiver.receive_for(wait);
        if (!batch.has_value()) {
            break;
        }
        for (const auto& event : batch.value()->value().events()) {
            if (event.has_access_log()) {
                logs.push_back(event.access_log());
            }
        }
    }
    co_return logs;
}
void verify() {
    origin source;
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [sender, receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 64});
    flexedge::testing::temporary_cache_directory directory;
    data_plane plane(workers, control.loop(0), runtime, sender, directory.path_);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    plane.start();
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::any(), 0}), tls_reservation(io, {asio::ip::address_v4::any(), 0});
    const auto port = reservation.local_endpoint().port();
    const auto tls_port = tls_reservation.local_endpoint().port();
    reservation.close();
    tls_reservation.close();
    auto config = flexedge::testing::snapshot(false);
    const auto identity = flexedge::testing::make_identity("www.example.com", 1);
    config.mutableCertificate(0)->set_certificate_chain_pem(identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(identity.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(tls_port);
    auto* website = config.mutableWebsite();
    website->set_access_log_response_headers(false);
    website->set_health_check_enabled(false);
    website->set_access_log_enabled(true);
    website->clear_access_log_status_code_ranges();
    website->mutable_origins(0)->set_host("127.0.0.1");
    website->mutable_origins(0)->set_port(source.port());
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    for (const bool websocket : {false, true}) {
        source.reset();
        asio::ip::tcp::socket socket(io);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto path = websocket ? "/ws" : "/events";
        const std::string request = std::string("GET ") + path + " HTTP/1.1\r\nHost: www.example.com\r\n" +
                                    (websocket ? "Connection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" : "Accept: text/event-stream\r\n") + "\r\n";
        asio::write(socket, asio::buffer(request));
        asio::streambuf response;
        asio::read_until(socket, response, "\r\n\r\n");
        std::string received(asio::buffers_begin(response.data()), asio::buffers_end(response.data()));
        if (!websocket) {
            require(received.starts_with("HTTP/1.1 103"), "informational response is forwarded before SSE");
            std::array<char, 1024> head{};
            while (received.find("\r\n\r\nHTTP/1.1 200 ") == std::string::npos) {
                const auto size = socket.read_some(asio::buffer(head));
                received.append(head.data(), size);
            }
        } else {
            require(received.starts_with("HTTP/1.1 101"), "WebSocket handshake succeeds");
        }
        std::string frame("\x81\x82\x00\x00\x00\x00hi", 8);
        if (websocket) {
            asio::write(socket, asio::buffer(frame));
            std::array<char, 8> echoed{};
            asio::read(socket, asio::buffer(echoed));
            require(std::string(echoed.data(), echoed.size()) == frame, "WebSocket tunnel forwards data");
            received.append(echoed.data(), echoed.size());
        }
        require(control.loop(0).start(collect(receiver, std::chrono::milliseconds(80))).get().empty(), "no access log at handshake or while streaming");
        source.finish();
        std::array<char, 1024> buffer{};
        for (;;) {
            std::error_code error;
            const auto size = socket.read_some(asio::buffer(buffer), error);
            received.append(buffer.data(), size);
            if (error) {
                break;
            }
        }
        const auto logs = control.loop(0).start(collect(receiver, std::chrono::milliseconds(200))).get();
        require(logs.size() == 1, "one final access log per connection");
        const auto& log = logs.front();
        require(log.request_type() == (websocket ? "websocket" : "sse"), "connection type survives without response-header capture");
        require(log.status_code() == (websocket ? 101u : 200u), "status retains the response sent to client");
        require(log.has_first_byte_ms() && log.total_duration_ms() >= log.first_byte_ms() + 50, "first byte differs from connection duration");
        require(log.started_unix_ms() <= log.occurred_unix_ms(), "log carries start and end time");
        require(log.total_response_bytes() == received.size(), "response bytes include tunnel data");
        require(log.total_request_bytes() == (websocket ? frame.size() : 0), "request bytes include tunnel data");
        require(log.end_reason() == (websocket ? "origin_closed" : "completed"), "final reason reports observed end");
    }
    shutdown.stop_and_join();
}
}  // namespace
int main() {
    try {
        verify();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
