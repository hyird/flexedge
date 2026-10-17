#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <set>
#include <string>
#include <thread>

#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/streambuf.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/cache_directory.h"
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
    explicit origin(std::string body = "body")
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          body_(std::move(body)) {
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
    void finish_stream() {
        release_stream_ = true;
    }
    void fail_stale(bool value) {
        fail_stale_ = value;
    }
    unsigned validations() const {
        return validations_;
    }
    bool stream_finished() const {
        return stream_finished_;
    }

private:
    struct stream_tail : std::enable_shared_from_this<stream_tail> {
        stream_tail(origin* owner, std::shared_ptr<asio::ip::tcp::socket> peer)
            : owner_(owner),
              peer_(std::move(peer)),
              timer_(peer_->get_executor()) {}
        void wait() {
            if (owner_->release_stream_) {
                auto bytes = std::make_shared<std::string>(16384, 'b');
                asio::async_write(*peer_, asio::buffer(*bytes), [self = shared_from_this(), bytes](std::error_code, std::size_t) {
                    self->owner_->stream_finished_ = true;
                    std::error_code ignored;
                    self->peer_->close(ignored);
                });
            } else {
                timer_.expires_after(std::chrono::milliseconds(5));
                timer_.async_wait([self = shared_from_this()](std::error_code error) { if (!error){ self->wait();
} });
            }
        }
        origin* owner_;
        std::shared_ptr<asio::ip::tcp::socket> peer_;
        asio::steady_timer timer_;
    };
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
                const std::string request_text(asio::buffers_begin(request->data()), asio::buffers_end(request->data()));
                if (request_text.starts_with("GET /origin-error ")) {
                    auto data = std::make_shared<std::string>("HTTP/1.1 502 Bad Gateway\r\nCache-Control: no-store\r\nContent-Type: text/plain\r\nContent-Length: 8\r\nConnection: close\r\n\r\nupstream");
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /read-timeout ")) {
                    auto timer = std::make_shared<asio::steady_timer>(io_);
                    timer->expires_after(std::chrono::milliseconds(1500));
                    timer->async_wait([peer, timer](std::error_code) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /ranges/")) {
                    constexpr std::size_t total = 3 * origin_range_block_bytes + 47;
                    const auto start = request_text.find("Range: ");
                    std::optional<cache_byte_range> selected;
                    if (start != std::string::npos && !request_text.starts_with("GET /ranges/ignore ")) {
                        const auto end = request_text.find("\r\n", start);
                        selected = parse_cache_range(std::string_view(request_text).substr(start + 7, end - start - 7), total);
                    }
                    const bool private_response = request_text.starts_with("GET /ranges/private ");
                    const auto offset = selected && selected->satisfiable_ ? selected->offset_ : 0;
                    const auto length = selected ? selected->length_ : total;
                    std::string body(length, '\0');
                    for (std::size_t index = 0; index < length; ++index) {
                        body[index] = static_cast<char>('a' + (offset + index) / origin_range_block_bytes);
                    }
                    std::string wire = selected ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                    wire += private_response ? "Cache-Control: private\r\n" : "Cache-Control: public, max-age=600\r\n";
                    wire += "ETag: \"ranges-v1\"\r\nContent-Type: application/octet-stream\r\n";
                    if (selected) {
                        wire += "Content-Range: bytes " + std::to_string(offset) + '-' + std::to_string(offset + length - 1) + '/' + std::to_string(total) + "\r\n";
                    }
                    wire += "Content-Length: " + std::to_string(length) + "\r\nConnection: close\r\n\r\n" + body;
                    auto data = std::make_shared<std::string>(std::move(wire));
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /no-cache ") || request_text.starts_with("GET /private ") || request_text.starts_with("GET /no-store ")) {
                    const auto control = request_text.starts_with("GET /private ") ? "private" : request_text.starts_with("GET /no-store ") ? "no-store"
                                                                                                                                            : "no-cache";
                    auto data = std::make_shared<std::string>(std::string("HTTP/1.1 200 OK\r\nCache-Control: ") + control +
                                                              "\r\nContent-Type: text/plain\r\nContent-Length: 4\r\nConnection: close\r\n\r\nbody");
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /revalidate ") || request_text.starts_with("GET /stale ") || request_text.starts_with("GET /updating ")) {
                    const bool validation = request_text.find("If-None-Match: \"cache-v1\"") != std::string::npos;
                    const bool origin_failure = request_text.starts_with("GET /stale ") && fail_stale_;
                    if (validation) {
                        ++validations_;
                    }
                    std::string wire;
                    if (origin_failure) {
                        wire = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
                    } else if (validation) {
                        wire = "HTTP/1.1 304 Not Modified\r\nETag: \"cache-v1\"\r\nCache-Control: public, max-age=1\r\nConnection: close\r\n\r\n";
                    } else {
                        wire = "HTTP/1.1 200 OK\r\nETag: \"cache-v1\"\r\nCache-Control: public, max-age=1\r\nContent-Type: text/plain\r\nContent-Length: 9\r\nConnection: close\r\n\r\nvalidated";
                        if (request_text.starts_with("GET /updating ")) {
                            wire.insert(wire.find("\r\nContent-Type"), ", stale-while-revalidate=60");
                        }
                    }
                    auto data = std::make_shared<std::string>(std::move(wire));
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) {std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /stream ")) {
                    auto deferred = std::make_shared<stream_tail>(this, peer);
                    auto first = std::make_shared<std::string>(
                        "HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\n"
                        "Connection: close\r\nContent-Type: application/octet-stream\r\nContent-Length: 32768\r\n\r\n" +
                        std::string(16384, 'a'));
                    asio::async_write(*peer, asio::buffer(*first), [deferred, first](std::error_code error, std::size_t) {
                        if (!error) {
                            deferred->wait();
                        }
                    });
                    return;
                }
                if (request_text.starts_with("GET /events ")) {
                    auto events = std::make_shared<std::string>(
                        "HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\n"
                        "Connection: close\r\nContent-Type: Text/Event-Stream; charset=utf-8\r\nContent-Length: 4096\r\n\r\n" +
                        std::string(4096, 'e'));
                    asio::async_write(*peer, asio::buffer(*events), [peer, events](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /large ")) {
                    auto bytes = std::make_shared<std::string>(
                        "HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\nETag: \"large\"\r\nContent-Type: application/octet-stream\r\nContent-Length: 524288\r\nConnection: close\r\n\r\n" +
                        std::string(262144, 'a') + std::string(262144, 'b'));
                    asio::async_write(*peer, asio::buffer(*bytes), [peer, bytes](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /date-range ") || request_text.starts_with("GET /weak-date-range ")) {
                    const auto date = cache_http_date(std::time(nullptr));
                    const auto modified = request_text.starts_with("GET /weak-date-range ") ? std::string_view(date) : "Wed, 21 Oct 2015 07:28:00 GMT";
                    auto data = std::make_shared<std::string>();
                    data->reserve(body_.size() + 256);
                    data->append("HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\nDate: ").append(date).append("\r\nLast-Modified: ").append(modified).append("\r\nContent-Length: ").append(std::to_string(body_.size())).append("\r\nConnection: close\r\n\r\n").append(body_);
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                if (request_text.starts_with("GET /old-date ")) {
                    auto data = std::make_shared<std::string>();
                    data->reserve(body_.size() + 256);
                    data->append("HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\nDate: Sun Nov  6 08:49:37 1994\r\nContent-Length: ")
                        .append(std::to_string(body_.size()))
                        .append("\r\nConnection: close\r\n\r\n")
                        .append(body_);
                    asio::async_write(*peer, asio::buffer(*data), [peer, data](std::error_code, std::size_t) { std::error_code ignored; peer->close(ignored); });
                    return;
                }
                const auto etag = request_text.starts_with("GET /etag-comma ") ? "\"test\\,part\"" : "\"test\"";
                auto bytes = std::make_shared<std::string>(
                    std::string("HTTP/1.1 200 OK\r\nCache-Control: public, max-age=600\r\nETag: ") + etag +
                    "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_);
                asio::async_write(*peer, asio::buffer(*bytes), [peer, bytes](std::error_code, std::size_t) {std::error_code ignored;peer->close(ignored); });
            });
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    const std::string body_;
    std::jthread thread_;
    std::atomic<unsigned> hits_{};
    std::atomic<bool> release_stream_{}, stream_finished_{}, fail_stale_{};
    std::atomic<unsigned> validations_{};
};
ruvia::task<void> activate(data_plane& plane, std::shared_ptr<const compiled_config> config) {
    auto pending = co_await config_activation::prepare(plane, std::move(config));
    co_await pending.commit();
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
                const auto& access = event.access_log();
                states.push_back(access.cache_revalidated() ? "revalidated" : access.cache_status());
                states.push_back(access.cache_reason());
            }
        }
    }
    co_return states;
}
void verify() {
    origin source;
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [logs, log_receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 128});
    flexedge::testing::temporary_cache_directory cache_directory;
    data_plane plane(workers, control.loop(0), runtime, logs, cache_directory.path_);
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
    site->mutable_domains(0)->set_hostname("*.example.com");
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
    rule.skip_request_cookie_ = true;
    rule.respect_request_no_cache_ = true;
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    auto request = [&](std::string_view headers = {}, std::string_view method = "GET", std::string_view target = "/asset") {
        return flexedge::testing::request(port, "www.example.com", headers, target, method);
    };
    const auto cache_usage = [&] {
        return control.loop(0).start(plane.report({})).get();
    };
    const auto empty_cache = cache_usage();
    FLEXEDGE_REQUIRE(empty_cache.load_ && empty_cache.cache_memory_bytes_ == 0 && empty_cache.cache_disk_bytes_ == 0);
    FLEXEDGE_REQUIRE(request().find("X-Cache: MISS") != std::string::npos);
    for (int index = 0; index < 20; ++index) {
        FLEXEDGE_REQUIRE(request().find("X-Cache: HIT") != std::string::npos);
    }
    const auto filled_cache = cache_usage();
    FLEXEDGE_REQUIRE(filled_cache.load_ && filled_cache.cache_memory_bytes_ > 4 && filled_cache.cache_disk_bytes_ > 4);
    FLEXEDGE_REQUIRE(source.hits() == 1);
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "assets.example.com", {}, "/asset").find("X-Cache: MISS") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == 2);
    const auto head = request({}, "HEAD");
    FLEXEDGE_REQUIRE(head.find("X-Cache: HIT") != std::string::npos && !head.ends_with("body"));
    FLEXEDGE_REQUIRE(request("If-None-Match: \"test\"\r\n").starts_with("HTTP/1.1 304"));
    const auto ranged = request("Range: bytes=1-2\r\n");
    FLEXEDGE_REQUIRE(ranged.starts_with("HTTP/1.1 206") && ranged.ends_with("od"));
    FLEXEDGE_REQUIRE(ranged.find("Content-Range: bytes 1-2/4") != std::string::npos);
    FLEXEDGE_REQUIRE(request("Range: bytes=-2\r\n").ends_with("dy"));
    FLEXEDGE_REQUIRE(request("Range: bytes=99-\r\n").starts_with("HTTP/1.1 416"));
    const auto if_range = request("Range: bytes=1-2\r\nIf-Range: \"different\"\r\n");
    FLEXEDGE_REQUIRE(if_range.starts_with("HTTP/1.1 200") && if_range.ends_with("body"));
    FLEXEDGE_REQUIRE(source.hits() == 2);
    FLEXEDGE_REQUIRE(request("Cookie: private=1\r\n").find("X-Cache: BYPASS") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == 3);
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
    FLEXEDGE_REQUIRE(result.ok() && result.affected_objects() == 2);
    const auto cleared_cache = cache_usage();
    FLEXEDGE_REQUIRE(cleared_cache.load_ && cleared_cache.cache_memory_bytes_ == 0 && cleared_cache.cache_disk_bytes_ == 0);
    FLEXEDGE_REQUIRE(request().find("X-Cache: MISS") != std::string::npos && source.hits() == 4);
    command.set_attempt_id("prewarm");
    command.set_operation("prewarm");
    command.set_scope("urls");
    command.add_urls("http://www.example.com/prewarm");
    result = control.loop(0).start(plane.execute_cache(command)).get();
    FLEXEDGE_REQUIRE(result.ok() && result.completed_urls() == 1);
    const auto before = source.hits();
    FLEXEDGE_REQUIRE(request("Accept-Encoding: identity\r\n", "GET", "/prewarm").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == before);
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "assets.example.com", "Accept-Encoding: identity\r\n", "/prewarm").find("X-Cache: MISS") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == before + 1);
    command.set_attempt_id("refresh-alias");
    command.set_operation("refresh");
    command.clear_urls();
    command.add_urls("http://assets.example.com/prewarm");
    result = control.loop(0).start(plane.execute_cache(command)).get();
    FLEXEDGE_REQUIRE(result.ok() && result.affected_objects() == 1);
    FLEXEDGE_REQUIRE(request("Accept-Encoding: identity\r\n", "GET", "/prewarm").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == before + 1);
    {
        const auto before_head = source.hits();
        const auto cold_head = request({}, "HEAD", "/cold-head");
        FLEXEDGE_REQUIRE(cold_head.find("X-Cache: MISS") != std::string::npos && cold_head.ends_with("\r\n\r\n"));
        FLEXEDGE_REQUIRE(request({}, "GET", "/cold-head").find("X-Cache: HIT") != std::string::npos);
        FLEXEDGE_REQUIRE(source.hits() == before_head + 1);
    }
    {
        const auto expected = std::string(262144, 'a') + std::string(262144, 'b');
        FLEXEDGE_REQUIRE(request({}, "GET", "/large").ends_with(expected));
        const auto origin_hits = source.hits();
        const auto cached = request({}, "GET", "/large");
        FLEXEDGE_REQUIRE(cached.find("X-Cache: HIT") != std::string::npos && cached.ends_with(expected));
        FLEXEDGE_REQUIRE(request("Range: bytes=262142-262145\r\n", "GET", "/large").ends_with("aabb"));
        const auto large_head = request({}, "HEAD", "/large");
        FLEXEDGE_REQUIRE(large_head.find("Content-Length: 524288\r\n") != std::string::npos && large_head.ends_with("\r\n\r\n"));
        FLEXEDGE_REQUIRE(request("If-None-Match: \"large\"\r\n", "GET", "/large").starts_with("HTTP/1.1 304"));
        {
            asio::io_context client_io;
            asio::ip::tcp::socket interrupted(client_io);
            interrupted.connect({asio::ip::address_v4::loopback(), port});
            const std::string wire = "GET /large HTTP/1.1\r\nHost: www.example.com\r\nConnection: close\r\n\r\n";
            asio::write(interrupted, asio::buffer(wire));
            std::array<char, 128> first;
            FLEXEDGE_REQUIRE(interrupted.read_some(asio::buffer(first)) > 0);
            interrupted.set_option(asio::socket_base::linger(true, 0));
            interrupted.close();
        }
        FLEXEDGE_REQUIRE(request({}, "GET", "/large").ends_with(expected));
        FLEXEDGE_REQUIRE(source.hits() == origin_hits);
        asio::io_context tls_io;
        asio::ssl::context tls_context(asio::ssl::context::tls_client);
        tls_context.set_verify_mode(asio::ssl::verify_none);
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            asio::ssl::stream<asio::ip::tcp::socket> encrypted(tls_io, tls_context);
            FLEXEDGE_REQUIRE(SSL_set_tlsext_host_name(encrypted.native_handle(), "www.example.com") == 1);
            encrypted.lowest_layer().connect({asio::ip::address_v4::loopback(), secure_port});
            encrypted.handshake(asio::ssl::stream_base::client);
            const std::string wire = "GET /large HTTP/1.1\r\nHost: www.example.com\r\nConnection: close\r\n\r\n";
            asio::write(encrypted, asio::buffer(wire));
            std::string received;
            std::array<char, 16384> bytes;
            std::error_code error;
            do {
                const auto count = encrypted.read_some(asio::buffer(bytes), error);
                received.append(bytes.data(), count);
            } while (!error);
            FLEXEDGE_REQUIRE(error == asio::error::eof || error == asio::ssl::error::stream_truncated);
            FLEXEDGE_REQUIRE(received.starts_with("HTTP/1.1 200 ") && received.ends_with(expected));
            if (attempt == 1) {
                FLEXEDGE_REQUIRE(received.find("X-Cache: HIT") != std::string::npos);
            }
        }
        FLEXEDGE_REQUIRE(source.hits() == origin_hits + 1);
    }
    policy.storage_ = "disk";
    site->set_revision(site->revision() + 1);
    site->clear_response_cache();
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.setGeneration(10);
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    FLEXEDGE_REQUIRE(request({}, "GET", "/disk").find("X-Cache: MISS") != std::string::npos);
    FLEXEDGE_REQUIRE(request({}, "GET", "/disk").find("X-Cache: HIT") != std::string::npos);
    {
        // Origin cannot finish until the client has observed its first body segment.
        asio::io_context client_io;
        asio::ip::tcp::socket client(client_io);
        client.connect({asio::ip::address_v4::loopback(), port});
        const std::string wire = "GET /stream HTTP/1.1\r\nHost: www.example.com\r\nConnection: close\r\n\r\n";
        asio::write(client, asio::buffer(wire));
        std::string received;
        std::array<char, 8192> buffer{};
        while (received.find("\r\n\r\n") == std::string::npos || received.ends_with("\r\n\r\n")) {
            const auto count = client.read_some(asio::buffer(buffer));
            received.append(buffer.data(), count);
        }
        FLEXEDGE_REQUIRE(!source.stream_finished());
        const auto while_filling = source.hits();
        auto follower = std::async(std::launch::async, [&] { return request({}, "GET", "/stream"); });
        std::this_thread::sleep_for(std::chrono::milliseconds(450));
        FLEXEDGE_REQUIRE(source.hits() == while_filling);
        source.finish_stream();
        for (;;) {
            std::error_code error;
            const auto count = client.read_some(asio::buffer(buffer), error);
            received.append(buffer.data(), count);
            if (error) {
                break;
            }
        }
        const auto separator = received.find("\r\n\r\n");
        FLEXEDGE_REQUIRE(received.substr(separator + 4) == std::string(16384, 'a') + std::string(16384, 'b'));
        const auto follower_response = follower.get();
        FLEXEDGE_REQUIRE(follower_response.find("X-Cache: HIT") != std::string::npos);
        const auto hits = source.hits();
        const auto cached = request({}, "GET", "/stream");
        FLEXEDGE_REQUIRE(cached.find("X-Cache: HIT") != std::string::npos && cached.ends_with(std::string(16384, 'b')));
        FLEXEDGE_REQUIRE(source.hits() == hits);
    }
    site->set_response_compression_enabled(true);
    site->set_revision(site->revision() + 1);
    config.setGeneration(11);
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    const auto before_events = source.hits();
    for (int index = 0; index < 2; ++index) {
        const auto events = request("Accept-Encoding: gzip\r\n", "GET", "/events");
        FLEXEDGE_REQUIRE(events.find("Content-Encoding:") == std::string::npos);
        FLEXEDGE_REQUIRE(events.ends_with(std::string(4096, 'e')));
        FLEXEDGE_REQUIRE(events.find("X-Cache: HIT") == std::string::npos);
    }
    FLEXEDGE_REQUIRE(source.hits() == before_events + 2);
    site->set_response_compression_enabled(false);
    site->set_revision(site->revision() + 1);
    policy.rules_[0].ttl_mode_ = "fixed";
    policy.rules_[0].ttl_seconds_ = 1;
    site->clear_response_cache();
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.setGeneration(12);
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    FLEXEDGE_REQUIRE(request({}, "GET", "/revalidate").ends_with("validated"));
    FLEXEDGE_REQUIRE(request({}, "GET", "/stale").ends_with("validated"));
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    const auto before_validation = source.validations();
    const auto validated = request({}, "GET", "/revalidate");
    if (validated.find("X-Cache: REVALIDATED") == std::string::npos) {
        std::cerr << validated.substr(0, validated.find("\r\n\r\n")) << '\n';
    }
    FLEXEDGE_REQUIRE(validated.starts_with("HTTP/1.1 200") && validated.ends_with("validated"));
    FLEXEDGE_REQUIRE(validated.find("X-Cache: REVALIDATED") != std::string::npos);
    FLEXEDGE_REQUIRE(source.validations() == before_validation + 1);
    source.fail_stale(true);
    const auto stale = request({}, "GET", "/stale");
    FLEXEDGE_REQUIRE(stale.starts_with("HTTP/1.1 200") && stale.ends_with("validated"));
    FLEXEDGE_REQUIRE(stale.find("X-Cache: STALE") != std::string::npos);
    const auto configure_rule = [&] {
        ++policy.revision_;
        site->set_revision(site->revision() + 1);
        site->clear_response_cache();
        flexedge::cache::to_proto(policy, *site->mutable_response_cache());
        config.setGeneration(config.release.generation() + 1);
        config.finalize();
        control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    };
    policy.rules_[0].ttl_seconds_ = 600;
    policy.rules_[0].ttl_mode_ = "origin";
    configure_rule();
    const auto before_no_cache_revalidation = source.hits();
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        FLEXEDGE_REQUIRE(request({}, "GET", "/no-cache").find("X-Cache: MISS") != std::string::npos);
    }
    FLEXEDGE_REQUIRE(source.hits() == before_no_cache_revalidation + 2);
    policy.rules_[0].ttl_mode_ = "fixed";
    policy.rules_[0].skip_no_cache_ = false;
    configure_rule();
    FLEXEDGE_REQUIRE(request({}, "GET", "/no-cache").find("X-Cache: MISS") != std::string::npos);
    const auto cached_no_cache = request({}, "GET", "/no-cache");
    FLEXEDGE_REQUIRE(cached_no_cache.find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(cached_no_cache.find("Cache-Control: no-cache") != std::string::npos);
    for (const auto directive : {"Cache-Control: no-cache\r\n", "Cache-Control: max-age=0\r\n", "Pragma: no-cache\r\n"}) {
        FLEXEDGE_REQUIRE(request(directive, "GET", "/no-cache").find("X-Cache: HIT") != std::string::npos);
    }
    FLEXEDGE_REQUIRE(request({}, "GET", "/no-cache").find("X-Cache: HIT") != std::string::npos);
    policy.rules_[0].respect_request_no_cache_ = false;
    configure_rule();
    FLEXEDGE_REQUIRE(request({}, "GET", "/no-cache").find("X-Cache: MISS") != std::string::npos);
    const auto before_ignored_refresh = source.hits();
    for (const auto directive : {"Cache-Control: no-cache\r\n", "Cache-Control: max-age=0\r\n", "Pragma: no-cache\r\n"}) {
        FLEXEDGE_REQUIRE(request(directive, "GET", "/no-cache").find("X-Cache: HIT") != std::string::npos);
    }
    FLEXEDGE_REQUIRE(source.hits() == before_ignored_refresh);
    FLEXEDGE_REQUIRE(request("Cookie: session=first\r\n", "GET", "/static.js").find("X-Cache: BYPASS") != std::string::npos);
    policy.rules_[0].skip_request_cookie_ = false;
    configure_rule();
    FLEXEDGE_REQUIRE(request("Cookie: session=first\r\n", "GET", "/static.js").find("X-Cache: MISS") != std::string::npos);
    const auto before_cookie_refresh = source.hits();
    for (const auto directive : {"", "Cache-Control: no-cache\r\n", "Cache-Control: max-age=0\r\n", "Pragma: no-cache\r\n"}) {
        FLEXEDGE_REQUIRE(request(std::string("Cookie: session=second\r\n") + directive, "GET", "/static.js").find("X-Cache: HIT") != std::string::npos);
    }
    FLEXEDGE_REQUIRE(request("Cookie: session=second\r\n", "HEAD", "/static.js").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(source.hits() == before_cookie_refresh);
    FLEXEDGE_REQUIRE(request("Cookie: session=first\r\nAuthorization: Bearer token\r\n", "GET", "/static.js").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(request("Cookie: session=first\r\nCache-Control: no-store\r\n", "GET", "/static.js").find("X-Cache: BYPASS") != std::string::npos);
    for (const auto target : {"/private", "/no-store"}) {
        const auto miss_response = request("Cookie: session=first\r\n", "GET", target);
        FLEXEDGE_REQUIRE(miss_response.find("X-Cache: MISS") != std::string::npos);
        const auto before_override_hit = source.hits();
        FLEXEDGE_REQUIRE(request("Cookie: session=second\r\n", "GET", target).find("X-Cache: HIT") != std::string::npos);
        FLEXEDGE_REQUIRE(source.hits() == before_override_hit);
    }
    policy.rules_[0].ttl_mode_ = "origin";
    configure_rule();
    for (const auto target : {"/private", "/no-store"}) {
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            FLEXEDGE_REQUIRE(request("Cookie: session=first\r\n", "GET", target).find("X-Cache: BYPASS") != std::string::npos);
        }
    }
    policy.rules_[0].ttl_mode_ = "origin";
    auto& updating_settings = policy.rules_[0].settings_.emplace();
    updating_settings.enabled_ = {"edge", "stale"};
    updating_settings.serve_stale_ = true;
    configure_rule();
    FLEXEDGE_REQUIRE(request({}, "GET", "/updating").find("X-Cache: MISS") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    const auto before_updating = source.validations();
    FLEXEDGE_REQUIRE(request({}, "GET", "/updating").find("X-Cache: UPDATING") != std::string::npos);
    for (unsigned attempt = 0; attempt < 50 && source.validations() == before_updating; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FLEXEDGE_REQUIRE(source.validations() == before_updating + 1);
    const auto refreshed_updating = request({}, "GET", "/updating");
    FLEXEDGE_REQUIRE(refreshed_updating.ends_with("validated"));
    updating_settings.serve_stale_ = false;
    configure_rule();
    FLEXEDGE_REQUIRE(request({}, "GET", "/updating").find("X-Cache: MISS") != std::string::npos);
    std::this_thread::sleep_for(std::chrono::milliseconds(1150));
    FLEXEDGE_REQUIRE(request({}, "GET", "/updating").find("X-Cache: REVALIDATED") != std::string::npos);
    {
        const auto cross = std::string("Range: bytes=") + std::to_string(origin_range_block_bytes - 2) + '-' + std::to_string(origin_range_block_bytes + 1) + "\r\n";
        const auto before_cold_range = source.hits();
        const auto cold = request(cross, "GET", "/ranges/off");
        FLEXEDGE_REQUIRE(cold.starts_with("HTTP/1.1 206") && cold.ends_with("aabb"));
        const auto complete = request({}, "GET", "/ranges/off");
        FLEXEDGE_REQUIRE(complete.find("X-Cache: HIT") != std::string::npos && source.hits() == before_cold_range + 1);
        const auto multipart = request("Range: bytes=0-1,1048576-1048577\r\n", "GET", "/ranges/off");
        FLEXEDGE_REQUIRE(multipart.find("multipart/byteranges; boundary=") != std::string::npos);
        FLEXEDGE_REQUIRE(multipart.find("\r\n\r\naa\r\n--") != std::string::npos && multipart.find("\r\n\r\nbb\r\n--") != std::string::npos);
        const auto range_head = request("Range: bytes=0-1\r\n", "HEAD", "/ranges/off");
        FLEXEDGE_REQUIRE(range_head.starts_with("HTTP/1.1 206") && range_head.ends_with("\r\n\r\n"));
        auto& settings = policy.rules_[0].settings_.emplace();
        settings.enabled_ = {"edge", "range"};
        settings.origin_range_ = "on";
        configure_rule();
        const auto before_blocks = source.hits();
        const auto partial = request(cross, "GET", "/ranges/on");
        FLEXEDGE_REQUIRE(partial.starts_with("HTTP/1.1 206") && partial.ends_with("aabb"));
        FLEXEDGE_REQUIRE(source.hits() == before_blocks + 2);
        const auto from_blocks = request(cross, "GET", "/ranges/on");
        FLEXEDGE_REQUIRE(from_blocks.find("X-Cache: HIT") != std::string::npos && from_blocks.ends_with("aabb"));
        FLEXEDGE_REQUIRE(source.hits() == before_blocks + 2);
        const auto suffix = request("Range: bytes=-5\r\n", "GET", "/ranges/on");
        FLEXEDGE_REQUIRE(suffix.starts_with("HTTP/1.1 206") && suffix.ends_with("ddddd"));
        const auto ignored = request(cross, "GET", "/ranges/ignore");
        FLEXEDGE_REQUIRE(ignored.starts_with("HTTP/1.1 206") && ignored.ends_with("aabb"));
        const auto before_ignore_hit = source.hits();
        FLEXEDGE_REQUIRE(request(cross, "GET", "/ranges/ignore").find("X-Cache: HIT") != std::string::npos);
        FLEXEDGE_REQUIRE(source.hits() == before_ignore_hit);
        const auto before_private = source.hits();
        for (int repeat = 0; repeat < 2; ++repeat) {
            const auto private_range = request(cross, "GET", "/ranges/private");
            FLEXEDGE_REQUIRE(private_range.starts_with("HTTP/1.1 206") && private_range.ends_with("aabb"));
        }
        FLEXEDGE_REQUIRE(source.hits() == before_private + 4);
        const auto mismatch = request("Range: bytes=1-3\r\nIf-Range: \"different\"\r\n", "GET", "/ranges/on");
        FLEXEDGE_REQUIRE(mismatch.starts_with("HTTP/1.1 200") && mismatch.size() - mismatch.find("\r\n\r\n") - 4 == 3 * origin_range_block_bytes + 47);
        settings.enabled_.push_back("errors");
        settings.error_passthrough_ = false;
        configure_rule();
        const auto replaced_error = request({}, "GET", "/origin-error");
        FLEXEDGE_REQUIRE(replaced_error.starts_with("HTTP/1.1 502") && replaced_error.ends_with("\r\n\r\n"));
        settings.error_passthrough_ = true;
        configure_rule();
        FLEXEDGE_REQUIRE(request({}, "GET", "/origin-error").ends_with("upstream"));
        const auto on_port = [&] { return flexedge::testing::request(port, "www.example.com:8888", {}, "/port.js"); };
        FLEXEDGE_REQUIRE(on_port().find("X-Cache: BYPASS") != std::string::npos);
        settings.enabled_.push_back("ports");
        settings.ports_ = {8888};
        configure_rule();
        FLEXEDGE_REQUIRE(on_port().find("X-Cache: MISS") != std::string::npos);
        FLEXEDGE_REQUIRE(on_port().find("X-Cache: HIT") != std::string::npos);
        settings.enabled_.push_back("timeout");
        settings.read_timeout_ = 1;
        configure_rule();
        const auto started = std::chrono::steady_clock::now();
        FLEXEDGE_REQUIRE(request({}, "GET", "/read-timeout").starts_with("HTTP/1.1 524"));
        FLEXEDGE_REQUIRE(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1400));
    }
    const auto states = control.loop(0).start(cache_log_states(log_receiver)).get();
    for (const auto state : {"miss", "hit_memory", "hit_disk", "revalidated", "bypass", "origin_not_modified", "origin_private", "origin_no_store", "fresh"}) {
        if (std::ranges::find(states, state) == states.end()) {
            std::cerr << "missing cache log state: " << state << '\n';
        }
        FLEXEDGE_REQUIRE(std::ranges::find(states, state) != states.end());
    }
    std::string large_body(3 * 65536 + 17, '\0');
    for (std::size_t index = 0; index < large_body.size(); ++index) {
        large_body[index] = static_cast<char>((index * 31 + index / 65536) % 251);
    }
    origin large_source(large_body);
    site->mutable_origins(0)->set_port(large_source.port());
    site->set_revision(site->revision() + 1);
    config.setGeneration(config.release.generation() + 1);
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    FLEXEDGE_REQUIRE(request({}, "GET", "/disk-large").find("X-Cache: MISS") != std::string::npos);
    const auto replay = request({}, "GET", "/disk-large");
    const auto head_end = replay.find("\r\n\r\n");
    FLEXEDGE_REQUIRE(head_end != std::string::npos);
    FLEXEDGE_REQUIRE(replay.substr(0, head_end).find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(replay.substr(head_end + 4) == large_body);
    const auto disk_head = request({}, "HEAD", "/disk-large");
    FLEXEDGE_REQUIRE(disk_head.substr(disk_head.find("\r\n\r\n") + 4).empty());
    const auto disk_unchanged = request("If-None-Match: \"test\"\r\n", "GET", "/disk-large");
    FLEXEDGE_REQUIRE(disk_unchanged.starts_with("HTTP/1.1 304"));
    FLEXEDGE_REQUIRE(disk_unchanged.substr(disk_unchanged.find("\r\n\r\n") + 4).empty());
    FLEXEDGE_REQUIRE(large_source.hits() == 1);
    const auto comma_miss = request({}, "GET", "/etag-comma");
    FLEXEDGE_REQUIRE(comma_miss.substr(comma_miss.find("\r\n\r\n") + 4) == large_body);
    const auto comma_match = request("If-None-Match: \"other\", W/\"test\\,part\"\r\n", "GET", "/etag-comma");
    FLEXEDGE_REQUIRE(comma_match.starts_with("HTTP/1.1 304"));
    FLEXEDGE_REQUIRE(comma_match.substr(comma_match.find("\r\n\r\n") + 4).empty());
    const auto invalid_match = request("If-None-Match: \"test\", invalid\r\n", "GET", "/disk-large");
    FLEXEDGE_REQUIRE(invalid_match.starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(invalid_match.substr(invalid_match.find("\r\n\r\n") + 4) == large_body);
    FLEXEDGE_REQUIRE(large_source.hits() == 2);
    const auto dated = request({}, "GET", "/date-range");
    FLEXEDGE_REQUIRE(dated.substr(dated.find("\r\n\r\n") + 4) == large_body);
    const auto later_date = request("Range: bytes=1-3\r\nIf-Range: Wed, 21 Oct 2015 07:28:01 GMT\r\n", "GET", "/date-range");
    FLEXEDGE_REQUIRE(later_date.starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(later_date.substr(later_date.find("\r\n\r\n") + 4) == large_body);
    const auto exact_date = request("Range: bytes=1-3\r\nIf-Range: Wed, 21 Oct 2015 07:28:00 GMT\r\n", "GET", "/date-range");
    FLEXEDGE_REQUIRE(exact_date.starts_with("HTTP/1.1 206"));
    FLEXEDGE_REQUIRE(exact_date.substr(exact_date.find("\r\n\r\n") + 4) == large_body.substr(1, 3));
    const auto weak_dated = request({}, "GET", "/weak-date-range");
    const auto weak_headers = std::string_view(weak_dated).substr(0, weak_dated.find("\r\n\r\n"));
    constexpr std::string_view modified_name = "last-modified: ";
    const auto modified_field = std::search(weak_headers.begin(), weak_headers.end(), modified_name.begin(), modified_name.end(),
        [](char actual, char expected) { return flexedge::route_policy::lower_ascii(actual) == expected; });
    FLEXEDGE_REQUIRE(modified_field != weak_headers.end());
    const auto modified_at = static_cast<std::size_t>(modified_field - weak_headers.begin());
    const auto modified_end = weak_dated.find("\r\n", modified_at);
    const auto weak_date = weak_dated.substr(modified_at + 15, modified_end - modified_at - 15);
    const auto weak_range = request("Range: bytes=1-3\r\nIf-Range: " + weak_date + "\r\n", "GET", "/weak-date-range");
    FLEXEDGE_REQUIRE(weak_range.starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(weak_range.substr(weak_range.find("\r\n\r\n") + 4) == large_body);
    FLEXEDGE_REQUIRE(large_source.hits() == 4);
    const auto legacy_modified = request("If-Modified-Since: Wednesday, 21-Oct-15 07:28:00 GMT\r\n", "GET", "/date-range");
    FLEXEDGE_REQUIRE(legacy_modified.starts_with("HTTP/1.1 304"));
    FLEXEDGE_REQUIRE(legacy_modified.substr(legacy_modified.find("\r\n\r\n") + 4).empty());
    const auto invalid_modified = request("If-Modified-Since: Wed, 21 Oct 2015 07:28:00 GMT junk\r\n", "GET", "/date-range");
    FLEXEDGE_REQUIRE(invalid_modified.starts_with("HTTP/1.1 200"));
    FLEXEDGE_REQUIRE(invalid_modified.substr(invalid_modified.find("\r\n\r\n") + 4) == large_body);
    const auto old_date_fetches = large_source.hits();
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        const auto old_date = request({}, "GET", "/old-date");
        FLEXEDGE_REQUIRE(old_date.substr(old_date.find("\r\n\r\n") + 4) == large_body);
    }
    FLEXEDGE_REQUIRE(large_source.hits() == old_date_fetches + 2);
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
