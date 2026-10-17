#include <atomic>
#include <iostream>
#include <set>
#include <thread>

#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

#define FLEXEDGE_REQUIRE(value)                                 \
    do {                                                        \
        if (!(value)) {                                         \
            throw std::runtime_error("cache runtime: " #value); \
        }                                                       \
    } while (false)
using namespace flexedge::node;
namespace {
class origin final {
public:
    origin()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}) {
        accept();
        thread_ = std::jthread([this] { io_.run(); });
    }
    ~origin() {
        asio::post(io_, [this] {acceptor_.close();io_.stop(); });
        thread_.join();
    }
    unsigned hits() const {
        return hits_;
    }
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            auto peer = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*peer, *request, "\r\n\r\n", [this, peer, request](std::error_code failure, std::size_t) {
                if (failure) {
                    return;
                }
                ++hits_;
                auto bytes = std::make_shared<std::string>(
                    "HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\nETag: \"test\"\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbody");
                asio::async_write(*peer, asio::buffer(*bytes), [peer, bytes](std::error_code, std::size_t) {std::error_code ignored;peer->close(ignored); });
            });
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::jthread thread_;
    std::atomic<unsigned> hits_{};
};
ruvia::task<void> activate(data_plane& plane, std::shared_ptr<const compiled_config> config) {
    auto pending = co_await plane.prepare(std::move(config));
    co_await plane.activate(pending);
}
ruvia::task<std::vector<std::string>> cache_log_states(ruvia::channel_receiver<owned_log_batch>& receiver) {
    std::vector<std::string> states;
    for (;;) {
        auto batch = co_await receiver.receive_for(std::chrono::milliseconds(50));
        if (!batch.has_value()) {
            break;
        }
        for (const auto& event : batch.value()->value().events()) {
            if (event.has_access_log()) {
                states.push_back(event.access_log().cache_status());
            }
        }
    }
    co_return states;
}
void verify() {
    origin source;
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [logs, log_receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 64});
    data_plane plane(workers, control.loop(0), runtime, logs);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    plane.start();
    asio::io_context reservations;
    asio::ip::tcp::acceptor http(reservations, {asio::ip::address_v4::any(), 0}), https(reservations, {asio::ip::address_v4::any(), 0});
    const auto port = http.local_endpoint().port(), secure_port = https.local_endpoint().port();
    http.close();
    https.close();
    auto config = flexedge::testing::snapshot(false);
    const auto certificate = flexedge::testing::make_identity("www.example.com", 1);
    config.mutableCertificate(0)->set_certificate_chain_pem(certificate.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(certificate.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(secure_port);
    auto* site = config.mutableWebsite();
    site->set_health_check_enabled(false);
    site->set_response_compression_enabled(false);
    site->mutable_origins(0)->set_host("127.0.0.1");
    site->mutable_origins(0)->set_port(source.port());
    flexedge::cache::policy policy;
    policy.id_ = "policy";
    flexedge::cache::rule rule;
    rule.id_ = "all";
    rule.name_ = "all";
    rule.match_type_ = "all";
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    auto request = [&](std::string_view headers = {}, std::string_view method = "GET", std::string_view target = "/asset") {
        return flexedge::testing::request(port, "www.example.com", headers, target, method);
    };
    FLEXEDGE_REQUIRE(request().find("X-Cache: MISS") != std::string::npos);
    for (int index = 0; index < 20; ++index) {
        FLEXEDGE_REQUIRE(request().find("X-Cache: HIT") != std::string::npos);
    }
    FLEXEDGE_REQUIRE(source.hits() == 1);
    const auto head = request({}, "HEAD");
    FLEXEDGE_REQUIRE(head.find("X-Cache: HIT") != std::string::npos && !head.ends_with("body"));
    FLEXEDGE_REQUIRE(request("If-None-Match: \"test\"\r\n").starts_with("HTTP/1.1 304"));
    FLEXEDGE_REQUIRE(request("Cookie: private=1\r\n").find("X-Cache: BYPASS") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == 2);
    v2::CacheCommand command;
    command.set_task_id("task");
    command.set_generation(1);
    command.set_attempt_id("refresh");
    command.set_operation("refresh");
    command.set_policy_id("policy");
    command.add_website_ids(site->id());
    command.set_scope("all");
    command.set_expires_unix_ms(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + 600000);
    auto result = control.loop(0).start(plane.execute_cache(command)).get();
    FLEXEDGE_REQUIRE(result.ok() && result.affected_objects() == 1);
    FLEXEDGE_REQUIRE(request().find("X-Cache: MISS") != std::string::npos && source.hits() == 3);
    command.set_attempt_id("prewarm");
    command.set_operation("prewarm");
    command.set_scope("urls");
    command.add_urls("http://www.example.com/prewarm");
    result = control.loop(0).start(plane.execute_cache(command)).get();
    FLEXEDGE_REQUIRE(result.ok() && result.completed_urls() == 1);
    const auto before = source.hits();
    FLEXEDGE_REQUIRE(request("Accept-Encoding: identity\r\n", "GET", "/prewarm").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == before);
    policy.storage_ = "disk";
    site->set_revision(site->revision() + 1);
    site->clear_response_cache();
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.setGeneration(10);
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    FLEXEDGE_REQUIRE(request({}, "GET", "/disk").find("X-Cache: MISS") != std::string::npos);
    FLEXEDGE_REQUIRE(request({}, "GET", "/disk").find("X-Cache: HIT") != std::string::npos);
    const auto states = control.loop(0).start(cache_log_states(log_receiver)).get();
    for (const auto state : {"miss", "hit_memory", "hit_disk", "bypass"}) {
        FLEXEDGE_REQUIRE(std::ranges::find(states, state) != states.end());
    }
    shutdown.stop_and_join();
}
}  // namespace
int main() {
    try {
        verify();
        std::cout << "cache runtime passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
