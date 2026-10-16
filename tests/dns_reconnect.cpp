#include <ruvia/core/async.h>
#include <ruvia/core/event_loop_pool.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "node/data/origin_connection_pool.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/dns_answers.h"
#include "tests/support/http_client.h"

#define FLEXEDGE_REQUIRE(condition)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;
using namespace std::chrono_literals;

class origin_server final {
public:
    origin_server(std::string_view address, std::uint16_t port = 0)
        : acceptor_(context_, {asio::ip::make_address(address), port}) {
        accept();
        thread_ = std::jthread([this] { context_.run(); });
    }
    ~origin_server() {
        asio::post(context_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& socket : sockets_) {
                socket->close(ignored);
            }
            context_.stop();
        });
        thread_.join();
    }
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    std::atomic<unsigned> connections_{};

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            sockets_.push_back(std::make_shared<asio::ip::tcp::socket>(std::move(socket)));
            ++connections_;
            accept();
        });
    }
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> sockets_{};
    std::jthread thread_;
};

class origin_client final {
public:
    origin_client(ruvia::event_loop loop, std::uint16_t port)
        : loop_(std::move(loop)),
          pool_(loop_.executor(), tls_),
          key_{.host_ = "flexedge-dns.test", .port_ = port},
          stop_registration_(loop_.on_stop([this]() -> ruvia::task<void> { co_await close(); })) {}
    ruvia::task<void> connect(bool idle) {
        held_ = pool_.acquire(key_);
        if (!held_->connected()) {
            const auto result = co_await ruvia::async_asio([this](auto done) {
                held_->connect(key_.host_, key_.port_, false, std::move(done));
            });
            FLEXEDGE_REQUIRE(!result.error_code());
        }
        if (idle) {
            pool_.release(key_, std::move(held_));
        }
    }
    ruvia::task<bool> reusable() {
        co_return held_ && held_->connected();
    }
    ruvia::task<void> finish() {
        pool_.release(key_, std::move(held_));
        co_return;
    }
    ruvia::task<void> write_in_flight() {
        const char byte = 'x';
        const auto written = co_await ruvia::async_asio<std::size_t>([this, &byte](auto done) {
            held_->write(asio::buffer(&byte, 1), std::move(done));
        });
        FLEXEDGE_REQUIRE(!written.error_code());
    }
    ruvia::task<void> tunnel() {
        held_->enter_tunnel();
        co_return;
    }
    ruvia::task<void> close() {
        held_.reset();
        pool_.close();
        co_return;
    }
    ruvia::event_loop loop_;
    origin_tls_context tls_;
    origin_connection_pool pool_;
    origin_connection_key key_;
    pooled_origin_connection held_{};
    ruvia::event_loop_stop_registration stop_registration_;
};

void verify_dns_reconnect() {
    const auto parent = std::filesystem::temp_directory_path();
    const auto parent_permissions = std::filesystem::status(parent).permissions();
    const auto path = parent /
                      ("flexedge-dns-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct directory_cleanup final {
        std::filesystem::path path_;
        ~directory_cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }
    } cleanup{path};
    flexedge::testing::dns_answers answers(path / "answers");
    FLEXEDGE_REQUIRE(std::filesystem::status(parent).permissions() == parent_permissions);
    origin_server first("127.0.0.1");
    origin_server second("127.0.0.2", first.port());
    ruvia::event_loop_pool workers({.loop_count_ = 1});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    origin_client client(workers.loop(0), first.port());
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    ruvia::stop_source stop;
    auto watching = client.loop_.start(client.pool_.watch_dns(client.loop_, stop.token(), 20ms));
    const auto acquire = [&](bool idle) { client.loop_.start(client.connect(idle)).get(); };
    const auto wait_obsolete = [&] {
        return flexedge::testing::waitUntil([&] { return !client.loop_.start(client.reusable()).get(); }, 2s);
    };
    acquire(true);
    acquire(false);
    FLEXEDGE_REQUIRE(flexedge::testing::waitUntil([&] { return first.connections_ == 1; }, 1s));
    answers.update("127.0.0.2");
    FLEXEDGE_REQUIRE(wait_obsolete());
    // An in-flight HTTP exchange can still write its old socket, then finishes normally.
    client.loop_.start(client.write_in_flight()).get();
    client.loop_.start(client.finish()).get();
    acquire(true);
    FLEXEDGE_REQUIRE(flexedge::testing::waitUntil([&] { return second.connections_ == 1; }, 1s));
    answers.update("error");
    client.loop_.start([](ruvia::worker_handle worker) -> ruvia::task<void> {
                    (void)co_await ruvia::sleep_for(worker, 100ms);
                }(client.loop_.handle()))
        .get();
    acquire(false);
    FLEXEDGE_REQUIRE(client.loop_.start(client.reusable()).get());
    FLEXEDGE_REQUIRE(second.connections_ == 1);

    // A long-lived tunnel has no request boundary at which to retire its old socket.
    client.loop_.start(client.tunnel()).get();
    answers.update("127.0.0.1,127.0.0.2");
    FLEXEDGE_REQUIRE(wait_obsolete());
    client.loop_.start(client.finish()).get();
    acquire(false);
    answers.update("127.0.0.2,127.0.0.1");
    client.loop_.start([](ruvia::worker_handle worker) -> ruvia::task<void> {
                    (void)co_await ruvia::sleep_for(worker, 100ms);
                }(client.loop_.handle()))
        .get();
    FLEXEDGE_REQUIRE(client.loop_.start(client.reusable()).get());
    client.loop_.start(client.finish()).get();
    stop.request_stop();
    watching.get();
    answers.update("slow");
    ruvia::stop_source cancel_lookup;
    auto resolving = client.loop_.start(resolve_dns(client.loop_, "flexedge-dns.test", first.port(), cancel_lookup.token()));
    client.loop_.start([]() -> ruvia::task<void> { co_return; }()).get();
    const auto cancelled_at = std::chrono::steady_clock::now();
    cancel_lookup.request_stop();
    FLEXEDGE_REQUIRE(!resolving.get().has_value());
    FLEXEDGE_REQUIRE(std::chrono::steady_clock::now() - cancelled_at < 1s);
    client.loop_.start(client.close()).get();
    shutdown.stop_and_join();
}
}  // namespace

int main() {
    try {
        verify_dns_reconnect();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
