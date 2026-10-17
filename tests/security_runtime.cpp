#include <atomic>
#include <iostream>
#include <thread>

#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "security_catalog.h"
#include "tests/support/cache_directory.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

#define FLEXEDGE_REQUIRE(value)               \
    do {                                      \
        if (!(value)) {                       \
            throw std::runtime_error(#value); \
        }                                     \
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
        asio::post(io_, [this] { acceptor_.close(); io_.stop(); });
        thread_.join();
    }
    auto port() const {
        return acceptor_.local_endpoint().port();
    }
    std::atomic<unsigned> hits_{};

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            auto peer = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            auto input = std::make_shared<asio::streambuf>();
            asio::async_read_until(*peer, *input, "\r\n\r\n", [this, peer, input](std::error_code failure, std::size_t) {
                if (failure) {
                    return;
                }
                ++hits_;
                auto bytes = std::make_shared<std::string>("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Type: text/plain\r\nCache-Control: public, max-age=600\r\nConnection: close\r\n\r\nok");
                asio::async_write(*peer, asio::buffer(*bytes), [peer, bytes](std::error_code, std::size_t) {
                    std::error_code ignored;
                    peer->close(ignored);
                });
            });
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::jthread thread_;
};
ruvia::task<void> activate(data_plane& plane, std::shared_ptr<const compiled_config> config) {
    auto pending = co_await config_activation::prepare(plane, std::move(config));
    co_await pending.commit();
}
ruvia::task<std::vector<v2::AccessLog>> access_logs(ruvia::channel_receiver<owned_log_batch>& receiver) {
    std::vector<v2::AccessLog> records;
    for (;;) {
        auto batch = co_await receiver.receive_for(std::chrono::milliseconds(100));
        if (!batch.has_value()) {
            break;
        }
        for (const auto& event : batch.value()->value().events()) {
            if (event.has_access_log()) {
                records.push_back(event.access_log());
            }
        }
    }
    co_return records;
}
void verify() {
    origin source;
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [logs, receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 128});
    flexedge::testing::temporary_cache_directory directory;
    data_plane plane(workers, control.loop(0), runtime, logs, directory.path_);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    plane.start();
    asio::io_context reservations;
    asio::ip::tcp::acceptor http(reservations, {asio::ip::address_v4::any(), 0}), https(reservations, {asio::ip::address_v4::any(), 0});
    const auto port = http.local_endpoint().port();
    const auto tls_port = https.local_endpoint().port();
    http.close();
    https.close();
    auto config = flexedge::testing::snapshot(false);
    const auto certificate = flexedge::testing::make_identity("www.example.com", 1);
    config.mutableCertificate(0)->set_certificate_chain_pem(certificate.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(certificate.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(tls_port);
    auto* website = config.mutableWebsite();
    website->set_health_check_enabled(false);
    website->set_access_log_enabled(true);
    website->clear_access_log_status_code_ranges();
    website->set_response_compression_enabled(false);
    website->mutable_origins(0)->set_host("127.0.0.1");
    website->mutable_origins(0)->set_port(source.port());
    auto* security = website->mutable_security();
    security->set_id("policy");
    security->set_name("Policy");
    security->set_revision(1);
    auto* managed = security->mutable_managed();
    managed->set_enabled(true);
    managed->set_paranoia_level(1);
    managed->set_inbound_threshold(5);
    managed->set_body_limit_bytes(1048576);
    auto* custom = security->add_custom_rules();
    custom->set_id("private");
    custom->set_enabled(true);
    custom->set_expression("http.request.uri.path eq \"/private\"");
    custom->set_action("block");
    custom->set_response_status(451);
    custom->set_response_content_type("application/json");
    custom->set_response_body("{\"blocked\":true}");
    auto* cc = security->add_cc_rules();
    cc->set_id("cc");
    cc->set_enabled(true);
    cc->set_expression("http.request.uri.path eq \"/login\"");
    cc->set_action("block");
    cc->set_requests(2);
    cc->set_window_seconds(10);
    cc->set_mitigation_seconds(1);
    flexedge::cache::policy cache;
    cache.id_ = "cache";
    cache.revision_ = 1;
    flexedge::cache::rule cache_rule;
    cache_rule.id_ = "all";
    cache_rule.name_ = "all";
    cache_rule.match_type_ = "all";
    cache.rules_.push_back(cache_rule);
    flexedge::cache::to_proto(cache, *website->mutable_response_cache());
    const auto publish = [&] {
        config.finalize();
        const auto snapshot = std::make_shared<const compiled_config>(config.active, config.objects);
        control.loop(0).start(activate(plane, snapshot)).get();
        return snapshot;
    };
    const auto pinned = publish();
    const auto request = [&](std::string_view path, std::string_view method = "GET", std::string_view body = {}) {
        return flexedge::testing::request(port, "www.example.com", "User-Agent: Mozilla/5.0\r\nAccept: text/html\r\nContent-Type: application/json\r\n", path, method, body);
    };
    FLEXEDGE_REQUIRE(request("/private").starts_with("HTTP/1.1 451"));
    FLEXEDGE_REQUIRE(source.hits_ == 0);
    FLEXEDGE_REQUIRE(request("/?q=1%27%20OR%201=1--").starts_with("HTTP/1.1 403"));
    FLEXEDGE_REQUIRE(source.hits_ == 0);
    FLEXEDGE_REQUIRE(request("/api", "POST", R"({"q":"<script>alert(1)</script>"})").starts_with("HTTP/1.1 403"));
    FLEXEDGE_REQUIRE(source.hits_ == 0);
    FLEXEDGE_REQUIRE(request("/asset").starts_with("HTTP/1.1 200"));
    const auto initial_logs = control.loop(0).start(access_logs(receiver)).get();
    bool clean_scored{}, attack_scored{}, custom_unscored{};
    for (const auto& entry : initial_logs) {
        std::cout << "Access log " << entry.target() << " status=" << entry.status_code()
                  << " score=" << (entry.has_waf_anomaly_score() ? std::to_string(entry.waf_anomaly_score()) : "absent") << '\n';
        if (entry.target() == "/asset") {
            clean_scored = entry.has_waf_anomaly_score() && entry.waf_anomaly_score() == 0;
        }
        if (entry.target().starts_with("/?q=")) {
            attack_scored = entry.has_waf_anomaly_score() && entry.waf_anomaly_score() >= 5 && entry.status_code() == 403;
        }
        if (entry.target() == "/private") {
            custom_unscored = !entry.has_waf_anomaly_score();
        }
    }
    FLEXEDGE_REQUIRE(clean_scored);
    FLEXEDGE_REQUIRE(attack_scored);
    FLEXEDGE_REQUIRE(custom_unscored);
    std::atomic<bool> concurrent_failure{};
    std::vector<std::jthread> clients;
    for (int index = 0; index < 8; ++index) {
        clients.emplace_back([&] {
            try {
                for (int attempt = 0; attempt < 20; ++attempt) {
                    if (!request("/?q=%3Cscript%3Ealert(1)%3C/script%3E").starts_with("HTTP/1.1 403")) {
                        concurrent_failure = true;
                    }
                }
            } catch (...) {
                concurrent_failure = true;
            }
        });
    }
    clients.clear();
    FLEXEDGE_REQUIRE(!concurrent_failure);
    const auto hits = source.hits_.load();
    FLEXEDGE_REQUIRE(request("/asset").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits_ == hits);
    bool limited{};
    for (int count = 0; count != 8; ++count) {
        if (request("/login").starts_with("HTTP/1.1 429")) {
            limited = true;
            break;
        }
    }
    FLEXEDGE_REQUIRE(limited);  // also protects cache hits
    // Exercise individual CRS activation through real HTTP, including a reload while
    // the preceding immutable ruleset is still retained by `pinned`.
    const auto set_detectors = [&](std::uint32_t enabled, std::uint64_t generation) {
        managed->clear_excluded_rules();
        for (const auto& detector : flexedge::security::crs_catalog) {
            if (detector.id_ != enabled) {
                managed->add_excluded_rules(detector.id_);
            }
        }
        security->set_revision(generation);
        config.setGeneration(generation);
        (void)publish();
    };
    set_detectors(0, 5);
    FLEXEDGE_REQUIRE(request("/?q=1%27%20OR%201=1--").starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(request("/api", "POST", R"({"q":"<script>alert(1)</script>"})").starts_with("HTTP/1.1 200"));
    set_detectors(942100, 6);
    FLEXEDGE_REQUIRE(request("/?q=1%27%20OR%201=1--").starts_with("HTTP/1.1 403"));
    FLEXEDGE_REQUIRE(request("/api", "POST", R"({"q":"<script>alert(1)</script>"})").starts_with("HTTP/1.1 200"));
    set_detectors(941100, 7);
    FLEXEDGE_REQUIRE(request("/?q=1%27%20OR%201=1--").starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(request("/api", "POST", R"({"q":"<script>alert(1)</script>"})").starts_with("HTTP/1.1 403"));
    managed->clear_excluded_rules();
    config.setGeneration(8);
    security->set_revision(8);
    cc->set_requests(100);
    custom->set_expression("http.request.uri.path eq \"/asset\"");
    (void)publish();
    FLEXEDGE_REQUIRE(request("/asset").starts_with("HTTP/1.1 451"));  // cached object cannot bypass new policy
    FLEXEDGE_REQUIRE(request("/login").starts_with("HTTP/1.1 200"));  // old mitigation cannot affect a new policy
    auto* list = security->add_lists();
    list->set_name("temporary_clients");
    list->set_type("ip");
    auto* temporary = list->add_members();
    temporary->set_value("127.0.0.1");
    temporary->set_expires_unix_ms(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) + 3000);
    list->add_entries("127.0.0.0/24");
    auto* listed = security->add_custom_rules();
    listed->set_id("listed");
    listed->set_enabled(true);
    listed->set_action("block");
    listed->set_expression("http.request.uri.path eq \"/temporary\" and ip.src in $temporary_clients");
    security->set_revision(9);
    config.setGeneration(9);
    (void)publish();
    FLEXEDGE_REQUIRE(request("/temporary").starts_with("HTTP/1.1 403"));
    list->clear_entries();
    security->set_revision(10);
    config.setGeneration(10);
    (void)publish();
    FLEXEDGE_REQUIRE(request("/temporary").starts_with("HTTP/1.1 403"));
    const auto expiry_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    bool expired{};
    do {
        if (request("/temporary").starts_with("HTTP/1.1 200")) {
            expired = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < expiry_deadline);
    FLEXEDGE_REQUIRE(expired);
    list->add_entries("127.0.0.0/24");
    security->set_revision(11);
    config.setGeneration(11);
    (void)publish();
    FLEXEDGE_REQUIRE(request("/temporary").starts_with("HTTP/1.1 403"));
    managed->set_inbound_threshold(1000);
    set_detectors(942100, 12);
    (void)control.loop(0).start(access_logs(receiver)).get();
    FLEXEDGE_REQUIRE(request("/score?q=1%27%20OR%201=1--").starts_with("HTTP/1.1 200"));
    const auto scored_logs = control.loop(0).start(access_logs(receiver)).get();
    FLEXEDGE_REQUIRE(std::ranges::any_of(scored_logs, [](const auto& entry) {
        return entry.target().starts_with("/score?") && entry.status_code() == 200 &&
               entry.has_waf_anomaly_score() && entry.waf_anomaly_score() == 5;
    }));
    shutdown.stop_and_join();
}
}  // namespace
int main() {
    try {
        verify();
        std::cout << "Security HTTP, cache ordering and reload isolation passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
