#include "node/data/data_worker.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>

#include "node/runtime/loop_shutdown.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

#define FLEXEDGE_REQUIRE(condition)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;
using flexedge::testing::TestConfig;
using flexedge::testing::waitUntil;

template <typename type>
concept writable_const_tls = requires(const type& context) { context.default_context(); };
static_assert(!writable_const_tls<tls_context_set>);
static_assert(std::is_same_v<owned_data_worker_request, std::unique_ptr<const data_worker_request>>);
static_assert(std::is_same_v<owned_data_worker_result, std::unique_ptr<const data_worker_result>>);

ruvia::task<std::shared_ptr<const origin_health_snapshot>> check_streaks(origin_health_supervisor& health) {
    const origin_health_key_set keys{{.website_id_ = "website", .origin_id_ = "origin"}};
    health.activate(health.prepare(keys, {}));
    const auto before = health.snapshot();
    const auto target = health.target("website", "origin");
    target.failure(2);
    target.success(2);
    target.failure(2);
    FLEXEDGE_REQUIRE(health.snapshot()->healthy({"website", "origin"}));
    target.failure(2);
    FLEXEDGE_REQUIRE(!health.snapshot()->healthy({"website", "origin"}));
    FLEXEDGE_REQUIRE(before->healthy({"website", "origin"}));
    const auto unhealthy = health.snapshot();
    auto pending = health.prepare(keys, {});
    target.success(2);
    target.success(2);
    health.activate(std::move(pending));
    FLEXEDGE_REQUIRE(health.snapshot()->healthy({"website", "origin"}));
    health.activate(health.prepare({}, {}));
    health.activate(health.prepare(keys, {}));
    target.failure(1);
    FLEXEDGE_REQUIRE(health.snapshot()->healthy({"website", "origin"}));
    co_return unhealthy;
}

void verify_streaks() {
    ruvia::event_loop_pool workers({.loop_count_ = 2});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    runtime_state first_runtime;
    runtime_state second_runtime;
    origin_tls_context first_tls;
    origin_tls_context second_tls;
    auto first = origin_health_supervisor::create(workers.loop(0), first_runtime, first_tls);
    auto second = origin_health_supervisor::create(workers.loop(1), second_runtime, second_tls);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    const auto unhealthy = workers.loop(0).start(check_streaks(*first)).get();
    const auto healthy = workers.loop(1).start([](origin_health_supervisor& local) -> ruvia::task<std::shared_ptr<const origin_health_snapshot>> {
                                            local.activate(local.prepare({{.website_id_ = "website", .origin_id_ = "origin"}}, {}));
                                            co_return local.snapshot();
                                        }(*second))
                             .get();
    FLEXEDGE_REQUIRE(!unhealthy->all_healthy());
    FLEXEDGE_REQUIRE(healthy->all_healthy());
    const std::array snapshots{healthy, unhealthy};
    const auto merged = origin_health_snapshot::merge(snapshots);
    FLEXEDGE_REQUIRE(!merged->all_healthy());
    FLEXEDGE_REQUIRE(merged->reports().front().status_ == "unhealthy");
    shutdown.stop_and_join();
}

// A bounded local origin stub: HEAD succeeds or stays in flight; GET closes without a head.
// Its counters are test instrumentation, independent of the node's worker state.
class origin_server final {
public:
    origin_server()
        : acceptor_(context_, {asio::ip::address_v4::loopback(), 0}) {
        accept();
        thread_ = std::jthread([this] { context_.run(); });
    }
    ~origin_server() {
        asio::post(context_, [this] {
            std::error_code ignored;
            acceptor_.close(ignored);
            for (const auto& session : held_) {
                session->socket_.close(ignored);
            }
            held_.clear();
            context_.stop();
        });
        thread_.join();
    }
    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    [[nodiscard]] unsigned heads() const {
        return heads_.load();
    }
    void hold_probes() {
        hold_ = true;
    }
    void hold_requests() {
        hold_requests_ = true;
    }
    [[nodiscard]] unsigned requests() const {
        return requests_.load();
    }
    void close_held() {
        asio::post(context_, [this] {
            std::error_code ignored;
            for (const auto& value : held_) {
                value->socket_.close(ignored);
            }
            held_.clear();
        });
    }
    void fail_held() {
        asio::post(context_, [this] {
            auto held = std::move(held_);
            held_.clear();
            for (const auto& session : held) {
                respond(session, "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
        });
    }

private:
    struct session final {
        explicit session(asio::ip::tcp::socket socket)
            : socket_(std::move(socket)) {}
        asio::ip::tcp::socket socket_;
        asio::streambuf head_{};
        std::string response_{};
    };
    static void respond(const std::shared_ptr<session>& value, std::string response) {
        value->response_ = std::move(response);
        asio::async_write(value->socket_, asio::buffer(value->response_), [value](std::error_code, std::size_t) {});
    }
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            auto value = std::make_shared<session>(std::move(socket));
            asio::async_read_until(value->socket_, value->head_, "\r\n\r\n", [this, value](std::error_code read_error, std::size_t) {
                if (read_error) {
                    return;
                }
                std::istream head(&value->head_);
                std::string method;
                head >> method;
                if (method == "HEAD") {
                    ++heads_;
                    if (hold_) {
                        held_.push_back(value);
                    } else {
                        respond(value, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    }
                } else {
                    ++requests_;
                    if (hold_requests_) {
                        held_.push_back(value);
                    }
                }
            });
            accept();
        });
    }
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    std::jthread thread_{};
    std::atomic<unsigned> heads_{};
    std::atomic<bool> hold_{};
    std::atomic<bool> hold_requests_{};
    std::atomic<unsigned> requests_{};
    std::vector<std::shared_ptr<session>> held_{};
};

ruvia::task<std::vector<owned_data_worker_result>> receive_results(
    ruvia::channel_receiver<owned_data_worker_result>& receiver, data_worker_operation operation,
    std::uint64_t sequence, std::size_t count) {
    std::vector<owned_data_worker_result> replies;
    while (replies.size() != count) {
        auto next = co_await receiver.receive_for(std::chrono::seconds(3));
        FLEXEDGE_REQUIRE(next.has_value());
        auto reply = std::move(next).take_value();
        FLEXEDGE_REQUIRE(reply->operation_ == operation && reply->sequence_ == sequence);
        FLEXEDGE_REQUIRE(std::ranges::none_of(replies, [&](const auto& existing) { return existing->worker_id_ == reply->worker_id_; }));
        replies.push_back(std::move(reply));
    }
    co_return replies;
}

void verify_workers() {
    constexpr std::size_t worker_count = 2;
    origin_server origin;
    ruvia::event_loop_pool loops({.loop_count_ = worker_count});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    auto [logs, log_receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 32});
    auto [results, receiver] = ruvia::make_channel<owned_data_worker_result>(control.loop(0).handle(), {.capacity_ = 16});
    auto cache = std::make_shared<response_cache_publication>();
    auto [cache_results, cache_receiver] = ruvia::make_channel<owned_data_worker_result>(control.loop(0).handle(), {.capacity_ = 16});
    std::vector<std::unique_ptr<data_worker>> workers;
    std::vector<ruvia::channel_sender<owned_data_worker_request>> requests;
    workers.reserve(worker_count);
    for (std::size_t identity = 0; identity != worker_count; ++identity) {
        auto [sender, input] = ruvia::make_channel<owned_data_worker_request>(loops.loop(identity).handle(), {.capacity_ = 16});
        workers.push_back(std::make_unique<data_worker>(loops.loop(identity), identity, std::move(input), results, logs, worker_count, cache, cache_results));
        requests.push_back(std::move(sender));
    }
    loop_shutdown shutdown(loops, control);
    loops.start();
    control.start();
    for (const auto& worker : workers) {
        worker->start();
    }
    std::uint64_t sequence{};
    const auto send = [&](data_worker_request request) {
        for (const auto& sender : requests) {
            FLEXEDGE_REQUIRE(sender.send(std::make_unique<const data_worker_request>(request)).accepted());
        }
    };
    const auto exchange = [&](data_worker_operation operation, std::shared_ptr<const compiled_config> config = {}) {
        const auto current = ++sequence;
        send({.operation_ = operation, .sequence_ = current, .config_ = std::move(config)});
        return control.loop(0).start(receive_results(receiver, operation, current, worker_count)).get();
    };
    const auto report = [&] {
        auto replies = exchange(data_worker_operation::report);
        for (const auto& reply : replies) {
            FLEXEDGE_REQUIRE(reply->ok_ && reply->snapshot_);
        }
        return replies;
    };
    const auto activate = [&](TestConfig config) {
        config.finalize();
        auto compiled = std::make_shared<const compiled_config>(config.active, config.objects);
        const auto current = ++sequence;
        send({.operation_ = data_worker_operation::prepare, .sequence_ = current, .config_ = compiled});
        for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::prepare, current, worker_count)).get()) {
            FLEXEDGE_REQUIRE(reply->ok_);
        }
        send({.operation_ = data_worker_operation::activate, .sequence_ = current});
        for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::activate, current, worker_count)).get()) {
            FLEXEDGE_REQUIRE(reply->ok_);
        }
        send({.operation_ = data_worker_operation::finalize, .sequence_ = current});
        return compiled;
    };

    asio::io_context clients;
    asio::ip::tcp::acceptor http_reservation(clients, {asio::ip::address_v4::any(), 0});
    asio::ip::tcp::acceptor https_reservation(clients, {asio::ip::address_v4::any(), 0});
    const auto http_port = http_reservation.local_endpoint().port();
    const auto https_port = https_reservation.local_endpoint().port();
    http_reservation.close();
    https_reservation.close();
    auto config = flexedge::testing::snapshot(false);
    const auto identity = flexedge::testing::make_identity("first.example.com", 1);
    config.mutableCertificate(0)->set_certificate_chain_pem(identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(identity.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(http_port);
    config.node_spec.mutable_endpoints(0)->set_https_port(https_port);
    auto* website = config.mutableWebsite();
    website->set_health_check_enabled(true);
    website->set_health_check_interval_seconds(60);
    website->set_healthy_threshold(1);
    website->set_unhealthy_threshold(1);
    website->mutable_origins(0)->set_host("127.0.0.1");
    website->mutable_origins(0)->set_port(origin.port());
    const auto initial = activate(config);
    FLEXEDGE_REQUIRE(waitUntil([&] {
        const auto snapshots = report();
        return origin.heads() == worker_count && std::ranges::all_of(snapshots, [](const auto& reply) {
            return reply->snapshot_->health_->reports().front().status_ == "healthy";
        });
    },
        std::chrono::seconds(4)));
    const auto healthy = report();

    // An identical source still receives a new paired execution view, but its participant
    // owns no replacement TLS, listeners or health membership. It is also reversible.
    auto reconnect = config;
    reconnect.finalize();
    const auto reconnect_config = std::make_shared<const compiled_config>(reconnect.active, reconnect.objects);
    const auto reconnect_sequence = ++sequence;
    send({.operation_ = data_worker_operation::prepare, .sequence_ = reconnect_sequence, .config_ = reconnect_config});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::prepare, reconnect_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_ && reply->added_listeners_ == 0 && reply->retired_listeners_ == 0);
    }
    send({.operation_ = data_worker_operation::activate, .sequence_ = reconnect_sequence});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::activate, reconnect_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == reconnect_config);
        const auto previous = std::ranges::find_if(healthy, [&](const auto& value) {
            return value->worker_id_ == reply->worker_id_;
        });
        FLEXEDGE_REQUIRE(previous != healthy.end());
        FLEXEDGE_REQUIRE(reply->snapshot_->health_ == (*previous)->snapshot_->health_);
    }
    send({.operation_ = data_worker_operation::rollback, .sequence_ = reconnect_sequence});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::rollback, reconnect_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == initial);
        FLEXEDGE_REQUIRE(reply->snapshot_->health_->all_healthy());
    }

    // One worker commits while the other rejects activation. Rollback must restore both
    // configs, health memberships, certificates and listener sets before the next attempt.
    asio::ip::tcp::acceptor rollback_port(clients, {asio::ip::address_v4::any(), 0});
    const auto rollback_https_port = rollback_port.local_endpoint().port();
    rollback_port.close();
    auto partial_reload = config;
    partial_reload.setGeneration(4);
    partial_reload.node_spec.mutable_endpoints(0)->set_https_port(rollback_https_port);
    partial_reload.mutableWebsite()->mutable_origins(0)->set_id("tentative-origin");
    const auto tentative_identity = flexedge::testing::make_identity("tentative.example.com", 3);
    partial_reload.mutableCertificate(0)->set_certificate_chain_pem(tentative_identity.certificate_);
    partial_reload.mutableCertificate(0)->set_private_key_pem(tentative_identity.private_key_);
    partial_reload.finalize();
    const auto rollback_sequence = ++sequence;
    send({.operation_ = data_worker_operation::prepare, .sequence_ = rollback_sequence, .config_ = std::make_shared<const compiled_config>(partial_reload.active, partial_reload.objects)});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::prepare, rollback_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    FLEXEDGE_REQUIRE(requests[1].send(std::make_unique<const data_worker_request>(data_worker_request{
                                          .operation_ = data_worker_operation::discard, .sequence_ = rollback_sequence}))
            .accepted());
    send({.operation_ = data_worker_operation::activate, .sequence_ = rollback_sequence});
    const auto partially_committed = control.loop(0).start(receive_results(receiver, data_worker_operation::activate, rollback_sequence, worker_count)).get();
    FLEXEDGE_REQUIRE(std::ranges::count_if(partially_committed, [](const auto& reply) { return reply->ok_; }) == 1);
    send({.operation_ = data_worker_operation::rollback, .sequence_ = rollback_sequence});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::rollback, rollback_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == initial);
        FLEXEDGE_REQUIRE(reply->snapshot_->health_->reports().front().origin_id_ == "origin-1");
        FLEXEDGE_REQUIRE(reply->snapshot_->health_->all_healthy());
    }
    send({.operation_ = data_worker_operation::activate, .sequence_ = rollback_sequence});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::activate, rollback_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(!reply->ok_ && reply->error_ == "data worker reload is already settled");
    }
    for (std::size_t attempt = 0; attempt != 4; ++attempt) {
        FLEXEDGE_REQUIRE(flexedge::testing::tls_request(https_port, "www.example.com", "unknown.example.com").peer_common_name_ == "first.example.com");
    }
    rollback_port.open(asio::ip::tcp::v4());
    rollback_port.bind({asio::ip::address_v4::any(), rollback_https_port});
    rollback_port.close();
    FLEXEDGE_REQUIRE(flexedge::testing::request(http_port).starts_with("HTTP/1.1 502 "));
    const auto failed = report();
    FLEXEDGE_REQUIRE(std::ranges::count_if(failed, [](const auto& reply) { return !reply->snapshot_->health_->all_healthy(); }) == 1);
    for (const auto& reply : healthy) {
        FLEXEDGE_REQUIRE(reply->snapshot_->health_->all_healthy());
    }

    asio::ip::tcp::socket partial(clients);
    partial.connect({asio::ip::address_v4::loopback(), http_port});
    const std::string prefix = "POST /upload HTTP/1.1\r\nHost: www.example.com\r\nContent-Length: 131072\r\n\r\n" + std::string(4096, 'x');
    asio::write(partial, asio::buffer(prefix));
    FLEXEDGE_REQUIRE(waitUntil([&] {
        const auto snapshots = report();
        return std::ranges::count_if(snapshots, [](const auto& reply) { return reply->snapshot_->request_buffer_bytes_ != 0; }) == 1;
    },
        std::chrono::seconds(2)));
    std::size_t request_limit{};
    std::size_t response_limit{};
    for (const auto& reply : report()) {
        request_limit += reply->snapshot_->request_buffer_limit_;
        response_limit += reply->snapshot_->response_buffer_limit_;
    }
    FLEXEDGE_REQUIRE(request_limit <= maximum_request_buffer_bytes);
    FLEXEDGE_REQUIRE(response_limit <= maximum_response_buffer_bytes);

    origin.hold_probes();
    config.setGeneration(4);
    config.mutableWebsite()->set_health_check_interval_seconds(1);
    activate(config);
    FLEXEDGE_REQUIRE(waitUntil([&] { return origin.heads() >= 2 * worker_count; }, std::chrono::seconds(3)));
    // An identical restore during a reconnect must preserve both in-flight probes.
    activate(config);
    origin.fail_held();
    FLEXEDGE_REQUIRE(waitUntil([&] {
        const auto snapshots = report();
        return std::ranges::all_of(snapshots, [](const auto& reply) { return !reply->snapshot_->health_->all_healthy(); });
    },
        std::chrono::seconds(2)));
    FLEXEDGE_REQUIRE(waitUntil([&] { return origin.heads() >= 3 * worker_count; }, std::chrono::seconds(3)));
    config.setGeneration(5);
    config.mutableWebsite()->set_health_check_enabled(false);
    config.mutableWebsite()->mutable_origins(0)->set_id("origin-replacement");
    const auto current = activate(config);
    origin.fail_held();
    for (const auto& reply : report()) {
        const auto values = reply->snapshot_->health_->reports();
        FLEXEDGE_REQUIRE(values.size() == 1 && values.front().origin_id_ == "origin-replacement");
        FLEXEDGE_REQUIRE(values.front().status_ == "unknown");
    }

    // A new attempt must also recover prepared sockets if a prior discard was not delivered.
    asio::ip::tcp::acceptor abandoned_port(clients, {asio::ip::address_v4::any(), 0});
    const auto abandoned_https_port = abandoned_port.local_endpoint().port();
    abandoned_port.close();
    auto abandoned = config;
    abandoned.setGeneration(6);
    abandoned.node_spec.mutable_endpoints(0)->set_https_port(abandoned_https_port);
    abandoned.finalize();
    for (const auto& reply : exchange(data_worker_operation::prepare, std::make_shared<const compiled_config>(abandoned.active, abandoned.objects))) {
        FLEXEDGE_REQUIRE(reply->ok_ && reply->added_listeners_ == 1 && reply->retired_listeners_ == 1);
    }
    origin.hold_requests();
    const auto old_requests = origin.requests();
    asio::ip::tcp::socket held_request(clients);
    held_request.connect({asio::ip::address_v4::loopback(), http_port});
    const std::string origin_request = "GET / HTTP/1.1\r\nHost: www.example.com\r\nConnection: close\r\n\r\n";
    asio::write(held_request, asio::buffer(origin_request));
    FLEXEDGE_REQUIRE(waitUntil([&] { return origin.requests() > old_requests; }, std::chrono::seconds(2)));
    auto invalid = config;
    invalid.setGeneration(6);
    invalid.mutableCertificate(0)->set_certificate_chain_pem("invalid certificate");
    invalid.finalize();
    const auto rejected = exchange(data_worker_operation::prepare, std::make_shared<const compiled_config>(invalid.active, invalid.objects));
    for (const auto& reply : rejected) {
        FLEXEDGE_REQUIRE(!reply->ok_);
    }
    send({.operation_ = data_worker_operation::discard, .sequence_ = sequence});
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == current);
    }
    abandoned_port.open(asio::ip::tcp::v4());
    abandoned_port.bind({asio::ip::address_v4::any(), abandoned_https_port});
    abandoned_port.close();

    asio::ssl::context client_tls(asio::ssl::context::tls_client);
    client_tls.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> held_tls(clients, client_tls);
    FLEXEDGE_REQUIRE(SSL_set_tlsext_host_name(held_tls.native_handle(), "www.example.com") == 1);
    held_tls.lowest_layer().connect({asio::ip::address_v4::loopback(), https_port});
    held_tls.handshake(asio::ssl::stream_base::client);
    const auto refreshed = flexedge::testing::make_identity("refreshed.example.com", 2);
    config.setGeneration(7);
    config.mutableCertificate(0)->set_certificate_chain_pem(refreshed.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(refreshed.private_key_);
    // Keep the business origin id but replace its endpoint. The old request's eventual
    // failure must be ignored by the replacement health membership on either worker.
    origin_server replacement_origin;
    replacement_origin.hold_probes();
    config.mutableWebsite()->mutable_origins(0)->set_port(replacement_origin.port());
    activate(config);
    origin.close_held();
    std::array<char, 4096> failed_request{};
    const auto failed_bytes = held_request.read_some(asio::buffer(failed_request));
    FLEXEDGE_REQUIRE(std::string_view(failed_request.data(), failed_bytes).starts_with("HTTP/1.1 502 "));
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->health_->reports().front().status_ == "unknown");
    }
    const std::string old_request = "GET / HTTP/1.1\r\nHost: unknown.example.com\r\nConnection: close\r\n\r\n";
    asio::write(held_tls, asio::buffer(old_request));
    std::array<char, 4096> response{};
    const auto received = held_tls.read_some(asio::buffer(response));
    FLEXEDGE_REQUIRE(std::string_view(response.data(), received).starts_with("HTTP/1.1 421 "));
    for (std::size_t attempt = 0; attempt != 4; ++attempt) {
        FLEXEDGE_REQUIRE(flexedge::testing::tls_request(https_port, "www.example.com", "unknown.example.com").peer_common_name_ == "refreshed.example.com");
    }
    const auto before_stop = replacement_origin.heads();
    config.setGeneration(8);
    config.mutableWebsite()->set_health_check_enabled(true);
    activate(config);
    FLEXEDGE_REQUIRE(waitUntil([&] { return replacement_origin.heads() >= before_stop + worker_count; }, std::chrono::seconds(3)));
    asio::ip::tcp::socket incomplete_tls(clients);
    incomplete_tls.connect({asio::ip::address_v4::loopback(), https_port});
    // Retire listeners while their sessions, TLS handshakes and origin callbacks are
    // still draining. Their worker keeps them alive through the subsequent stop/join.
    config.setGeneration(9);
    config.node_spec.set_enabled(false);
    activate(config);
    // The global commit can race admission stop before its final cleanup message reaches
    // one worker. Stop must never undo that worker's already acknowledged activation.
    config.setGeneration(10);
    config.finalize();
    const auto stopped_config = std::make_shared<const compiled_config>(config.active, config.objects);
    const auto stopped_sequence = ++sequence;
    send({.operation_ = data_worker_operation::prepare, .sequence_ = stopped_sequence, .config_ = stopped_config});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::prepare, stopped_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    send({.operation_ = data_worker_operation::activate, .sequence_ = stopped_sequence});
    for (const auto& reply : control.loop(0).start(receive_results(receiver, data_worker_operation::activate, stopped_sequence, worker_count)).get()) {
        FLEXEDGE_REQUIRE(reply->ok_);
    }
    FLEXEDGE_REQUIRE(requests[0].send(std::make_unique<const data_worker_request>(data_worker_request{
                                          .operation_ = data_worker_operation::finalize, .sequence_ = stopped_sequence}))
            .accepted());
    for (const auto& worker : workers) {
        worker->request_stop_accepting();
    }
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == stopped_config);
    }
    const auto decision_sequence = ++sequence;
    send({.operation_ = data_worker_operation::report, .sequence_ = decision_sequence, .committed_sequence_ = stopped_sequence});
    (void)control.loop(0).start(receive_results(receiver, data_worker_operation::report, decision_sequence, worker_count)).get();
    send({.operation_ = data_worker_operation::rollback, .sequence_ = stopped_sequence});
    (void)control.loop(0).start(receive_results(receiver, data_worker_operation::rollback, stopped_sequence, worker_count)).get();
    for (const auto& reply : report()) {
        FLEXEDGE_REQUIRE(reply->snapshot_->config_ == stopped_config);
    }
    const auto after_stop = exchange(data_worker_operation::prepare, initial);
    for (const auto& reply : after_stop) {
        FLEXEDGE_REQUIRE(!reply->ok_ && reply->error_ == "data worker is stopping");
    }
    const auto stopped_at = std::chrono::steady_clock::now();
    shutdown.stop_and_join();
    FLEXEDGE_REQUIRE(std::chrono::steady_clock::now() - stopped_at < std::chrono::seconds(3));
}
}  // namespace

int main() {
    try {
        verify_streaks();
        verify_workers();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
