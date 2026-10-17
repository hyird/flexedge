#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/task.h>
#include <ruvia/http/http_content_codec.h>
#include <ruvia/http/http_header.h>
#include <ruvia/http/http_status.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "common/file_digest.h"
#include "node/data/data_plane.h"
#include "node/data/edge_listener.h"
#include "node/data/health_supervisor.h"
#include "node/data/origin_health.h"
#include "node/data/origin_response_codec.h"
#include "node/data/origin_selection.h"
#include "node/data/worker_metrics.h"
#include "node/proto/release_metadata.h"
#include "node/proto/schema_version.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/connected_clients.h"
#include "node/runtime/http_redirect.h"
#include "node/runtime/log_producer.h"
#include "node/runtime/loop_shutdown.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/runtime_metrics.h"
#include "node/runtime/runtime_state.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/state_cipher.h"
#include "node/runtime/state_store.h"
#include "node/runtime/upgrade_record.h"
#include "tests/support/cache_directory.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {
static_assert(!std::is_copy_constructible_v<flexedge::node::compiled_config>);
static_assert(!std::is_copy_assignable_v<flexedge::node::compiled_config>);
static_assert(!std::is_move_constructible_v<flexedge::node::compiled_config>);
static_assert(!std::is_move_assignable_v<flexedge::node::compiled_config>);
static_assert(!std::is_copy_constructible_v<flexedge::node::tls_context_set>);
static_assert(!std::is_move_constructible_v<flexedge::node::tls_context_set>);
static_assert(!std::is_move_assignable_v<flexedge::node::tls_context_set>);
static_assert(!std::is_copy_constructible_v<flexedge::node::log_producer>);
static_assert(std::is_same_v<flexedge::node::owned_log_batch,
    std::unique_ptr<const flexedge::node::log_batch>>);
static_assert(std::is_same_v<decltype(std::declval<const flexedge::node::log_batch&>().value()),
    const flexedge::node::v2::LogDelivery&>);

using flexedge::testing::snapshot;
using flexedge::testing::TestConfig;

std::shared_ptr<const flexedge::node::compiled_config> compiled(TestConfig value) {
    value.finalize();
    return std::make_shared<const flexedge::node::compiled_config>(value.active, value.objects);
}

void apply(flexedge::node::runtime_state& runtime, TestConfig value) {
    auto config = compiled(std::move(value));
    runtime.validate_next(*config);
    runtime.publish_execution(std::make_shared<const flexedge::node::worker_execution_view>(
        std::move(config)));
}

using flexedge::testing::request;
using flexedge::testing::waitUntil;

ruvia::task<void> flush_logs(flexedge::node::log_producer& logs) {
    (void)logs.flush();
    co_return;
}

ruvia::task<std::vector<flexedge::node::v2::NodeLog>> receive_node_logs(
    ruvia::channel_receiver<flexedge::node::owned_log_batch>& receiver) {
    std::vector<flexedge::node::v2::NodeLog> result;
    for (;;) {
        auto batch = co_await receiver.receive_for(std::chrono::milliseconds(10));
        if (!batch.has_value()) {
            break;
        }
        for (const auto& event : batch.value()->value().events()) {
            if (event.has_node_log()) {
                result.push_back(event.node_log());
            }
        }
    }
    co_return result;
}

ruvia::task<void> verify_log_producer(flexedge::node::log_producer& logs,
    std::function<void(flexedge::node::log_producer&)> verify) {
    verify(logs);
    co_return;
}

ruvia::task<flexedge::node::owned_log_batch> receive_log_batch(
    ruvia::channel_receiver<flexedge::node::owned_log_batch>& receiver) {
    auto result = co_await receiver.receive_for(std::chrono::seconds(1));
    REQUIRE(result.has_value());
    co_return std::move(result).take_value();
}

ruvia::task<void> append_worker_logs(flexedge::node::log_producer& logs,
    std::string category, std::size_t count) {
    const auto before = logs.snapshot();
    REQUIRE(before.queued_events_ == 0);
    for (std::size_t index = 0; index < count; ++index) {
        REQUIRE(logs.node("info", category, std::to_string(index)));
    }
    REQUIRE(logs.flush());
    REQUIRE(logs.queued_events() == 0);
    REQUIRE(logs.dropped_events() == 0);
    const auto after = logs.snapshot();
    REQUIRE(after.published_events_ == before.published_events_ + count);
    REQUIRE(after.queued_events_ == 0);
    REQUIRE(after.dropped_events_ == 0);
    co_return;
}

ruvia::task<void> publish_temporary_logs(ruvia::event_loop owner,
    ruvia::channel_sender<flexedge::node::owned_log_batch> sender) {
    flexedge::node::log_producer logs(std::move(owner), std::move(sender));
    REQUIRE(logs.node("info", "lifetime", "owned by receiver"));
    REQUIRE(logs.flush());
    const auto production = logs.snapshot();
    REQUIRE(production.published_events_ == 1);
    REQUIRE(production.queued_events_ == 0);
    REQUIRE(production.dropped_events_ == 0);
    co_return;
}

void require_log_batch_bounds(const flexedge::node::log_batch& batch) {
    auto wire = batch.value();
    wire.set_node_id(std::string(32, 'a'));
    REQUIRE(wire.ByteSizeLong() <= flexedge::node::log_contract::max_delivery_bytes);
}

void verify_log_batches() {
    using namespace flexedge::node;
    ruvia::event_loop_pool workers({.loop_count_ = 2});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    const auto owner = control.loop(0);
    auto [sender, receiver] =
        ruvia::make_channel<owned_log_batch>(owner.handle(), {.capacity_ = 32});
    log_producer first_logs(workers.loop(0), sender);
    log_producer second_logs(workers.loop(1), sender);
    auto [bounded_sender, bounded_receiver] =
        ruvia::make_channel<owned_log_batch>(owner.handle(), {.capacity_ = 1});
    log_producer bounded_logs(workers.loop(0), bounded_sender);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();

    workers.loop(0).start(verify_log_producer(first_logs, [](log_producer& logs) {
                       REQUIRE(logs.access({
                           .website_id_ = "89cbd310-6e73-4e47-b020-0863ed440880",
                           .client_ip_ = "192.0.2.1",
                           .protocol_ = "HTTP/1.1",
                           .method_ = "GET",
                           .host_ = "www.example.com",
                           .target_ = "/",
                           .status_code_ = 200,
                           .user_agent_ = "test-agent",
                           .referer_ = "https://example.com/",
                           .request_headers_ = "Host: www.example.com\n",
                           .request_body_ = "payload",
                           .tls_fingerprint_ = "ja3:0123456789abcdef0123456789abcdef",
                           .response_headers_ = "Content-Type: text/plain\n",
                           .query_string_ = "debug=1",
                           .cookies_ = "sid=abc",
                       }));
                       REQUIRE(logs.node("warning", "control", "batched event"));
                       REQUIRE(logs.queued_events() == 2);
                       const auto production = logs.snapshot();
                       REQUIRE(production.published_events_ == 0);
                       REQUIRE(production.queued_events_ == 2);
                       REQUIRE(production.dropped_events_ == 0);
                       // Both records seal together in the deferred flush after this turn.
                   }))
        .get();
    auto delivery = owner.start(receive_log_batch(receiver)).get();
    REQUIRE(delivery->value().node_id().empty());
    REQUIRE(delivery->value().events_size() == 2);
    const auto& access = delivery->value().events(0).access_log();
    REQUIRE(!access.id().empty());
    REQUIRE(access.request_headers().find("Host:") != std::string::npos);
    REQUIRE(access.request_body() == "payload");
    REQUIRE(access.tls_fingerprint().starts_with("ja3:"));
    REQUIRE(access.response_headers().find("Content-Type:") != std::string::npos);
    REQUIRE(access.query_string() == "debug=1");
    REQUIRE(access.cookies() == "sid=abc");
    REQUIRE(delivery->value().events(1).node_log().message() == "batched event");
    const auto immutable_bytes = delivery->value().SerializeAsString();

    workers.loop(0).start(publish_temporary_logs(workers.loop(0), sender)).get();
    auto temporary = owner.start(receive_log_batch(receiver)).get();
    REQUIRE(temporary->value().events_size() == 1);
    REQUIRE(temporary->value().events(0).node_log().message() == "owned by receiver");
    REQUIRE(delivery->value().SerializeAsString() == immutable_bytes);

    workers.loop(0).start(append_worker_logs(first_logs, "load", 600)).get();
    auto full_batch = owner.start(receive_log_batch(receiver)).get();
    auto partial_batch = owner.start(receive_log_batch(receiver)).get();
    REQUIRE(full_batch->value().events_size() == 512);
    REQUIRE(partial_batch->value().events_size() == 88);
    require_log_batch_bounds(*full_batch);
    require_log_batch_bounds(*partial_batch);

    // A full channel drops only the new batch and leaves the accepted payload intact.
    workers.loop(0).start(verify_log_producer(bounded_logs, [](log_producer& logs) {
                       REQUIRE(logs.node("info", "capacity", "accepted"));
                       REQUIRE(logs.flush());
                       REQUIRE(logs.node("info", "capacity", "rejected"));
                       REQUIRE(!logs.flush());
                       REQUIRE(logs.queued_events() == 0);
                       REQUIRE(logs.dropped_events() == 1);
                       const auto production = logs.snapshot();
                       REQUIRE(production.published_events_ == 1);
                       REQUIRE(production.queued_events_ == 0);
                       REQUIRE(production.dropped_events_ == 1);
                   }))
        .get();
    auto accepted = owner.start(receive_log_batch(bounded_receiver)).get();
    REQUIRE(accepted->value().events_size() == 1);
    REQUIRE(accepted->value().events(0).node_log().message() == "accepted");
    workers.loop(0).start(verify_log_producer(bounded_logs, [](log_producer& logs) {
                       REQUIRE(logs.node("info", "capacity", "after drain"));
                       REQUIRE(logs.flush());
                       REQUIRE(logs.dropped_events() == 1);
                       const auto production = logs.snapshot();
                       REQUIRE(production.published_events_ == 2);
                       REQUIRE(production.queued_events_ == 0);
                       REQUIRE(production.dropped_events_ == 1);
                   }))
        .get();
    auto after_drain = owner.start(receive_log_batch(bounded_receiver)).get();
    REQUIRE(after_drain->value().events(0).node_log().message() == "after drain");

    // Each worker builds its own batches; only immutable ownership crosses the channel.
    auto first = workers.loop(0).start(append_worker_logs(first_logs, "worker-0", 2000));
    auto second = workers.loop(1).start(append_worker_logs(second_logs, "worker-1", 2000));
    first.get();
    second.get();
    std::array<std::size_t, 2> observed{};
    for (std::size_t batch_index = 0; batch_index < 8; ++batch_index) {
        auto batch = owner.start(receive_log_batch(receiver)).get();
        REQUIRE(batch->value().events_size() <= static_cast<int>(log_contract::max_delivery_events));
        require_log_batch_bounds(*batch);
        const auto& category = batch->value().events(0).node_log().category();
        REQUIRE(category == "worker-0" || category == "worker-1");
        const auto worker_index = category == "worker-0" ? std::size_t{0} : std::size_t{1};
        for (const auto& event : batch->value().events()) {
            REQUIRE(event.has_node_log());
            REQUIRE(event.node_log().category() == category);
            REQUIRE(event.node_log().message() == std::to_string(observed[worker_index]++));
        }
    }
    REQUIRE(observed[0] == 2000);
    REQUIRE(observed[1] == 2000);

    // Large access events split by wire bytes before reaching the event-count limit.
    workers.loop(0).start(verify_log_producer(first_logs, [](log_producer& logs) {
                       for (std::size_t index = 0; index < 2; ++index) {
                           REQUIRE(logs.access({
                               .website_id_ = "89cbd310-6e73-4e47-b020-0863ed440880",
                               .client_ip_ = "192.0.2.1",
                               .protocol_ = "HTTP/1.1",
                               .method_ = "POST",
                               .host_ = "www.example.com",
                               .target_ = "/upload",
                               .status_code_ = 200,
                               .user_agent_ = {},
                               .referer_ = {},
                               .request_headers_ = {},
                               .request_body_ = std::string(2 * 1024 * 1024, 'x'),
                               .tls_fingerprint_ = {},
                               .response_headers_ = {},
                               .query_string_ = {},
                               .cookies_ = {},
                           }));
                       }
                       REQUIRE(logs.flush());
                       REQUIRE(logs.dropped_events() == 0);
                   }))
        .get();
    for (std::size_t index = 0; index < 2; ++index) {
        auto batch = owner.start(receive_log_batch(receiver)).get();
        REQUIRE(batch->value().events_size() == 1);
        const auto& bounded_access = batch->value().events(0).access_log();
        REQUIRE(bounded_access.request_body().size() == log_producer::max_request_body_bytes);
        REQUIRE(bounded_access.request_body_truncated());
        require_log_batch_bounds(*batch);
    }
    REQUIRE(delivery->value().SerializeAsString() == immutable_bytes);
    v2::Website website_log_policy;
    REQUIRE(!website_log_policy.access_log_enabled());
    website_log_policy.set_access_log_enabled(true);
    REQUIRE(website_log_policy.access_log_enabled());
    shutdown.stop_and_join();
}

bool accept_until_stopped(asio::ip::tcp::acceptor& acceptor, asio::ip::tcp::socket& socket,
    std::stop_token stop_token) {
    std::error_code error;
    acceptor.non_blocking(true, error);
    if (error) {
        throw std::system_error(error, "could not make test acceptor non-blocking");
    }
    while (!stop_token.stop_requested()) {
        acceptor.accept(socket, error);
        if (!error) {
            return true;
        }
        if (error != asio::error::would_block && error != asio::error::try_again) {
            throw std::system_error(error, "could not accept test origin connection");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

using flexedge::testing::make_identity;
using flexedge::testing::tls_request;

std::string tls_alpn(std::uint16_t port, std::string_view server_name, bool offer_http2) {
    asio::io_context context;
    asio::ssl::context tls_context(asio::ssl::context::tls_client);
    tls_context.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tls_context);
    REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), std::string(server_name).c_str()) == 1);
    static constexpr unsigned char http1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    static constexpr unsigned char adaptive[] = {2, 'h', '2', 8, 'h', 't',
        't', 'p', '/', '1', '.', '1'};
    REQUIRE(SSL_set_alpn_protos(
                stream.native_handle(), offer_http2 ? adaptive : http1,
                static_cast<unsigned int>(offer_http2 ? sizeof(adaptive) : sizeof(http1))) == 0);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const unsigned char* selected{};
    unsigned int selected_size{};
    SSL_get0_alpn_selected(stream.native_handle(), &selected, &selected_size);
    return std::string(reinterpret_cast<const char*>(selected), selected_size);
}

using tls_session = std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)>;

tls_session reusable_tls_session(std::uint16_t port, SSL_SESSION* previous = nullptr) {
    asio::io_context context;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.set_verify_mode(asio::ssl::verify_none);
    REQUIRE(SSL_CTX_set_max_proto_version(tls.native_handle(), TLS1_2_VERSION) == 1);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tls);
    REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), "www.example.com") == 1);
    if (previous != nullptr) {
        REQUIRE(SSL_set_session(stream.native_handle(), previous) == 1);
    }
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    if (previous != nullptr) {
        REQUIRE(SSL_session_reused(stream.native_handle()) == 1);
    }
    tls_session session(SSL_get1_session(stream.native_handle()), &SSL_SESSION_free);
    REQUIRE(session && SSL_SESSION_is_resumable(session.get()) == 1);
    SSL_set_shutdown(stream.native_handle(), SSL_SENT_SHUTDOWN | SSL_RECEIVED_SHUTDOWN);
    return session;
}

void require_tls_rejection(std::uint16_t port, std::string_view server_name, int version, int expected_alert,
    SSL_SESSION* previous = nullptr) {
    asio::io_context context;
    asio::ssl::context tls(asio::ssl::context::tls_client);
    tls.set_verify_mode(asio::ssl::verify_none);
    REQUIRE(SSL_CTX_set_min_proto_version(tls.native_handle(), version) == 1);
    REQUIRE(SSL_CTX_set_max_proto_version(tls.native_handle(), version) == 1);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tls);
    if (previous != nullptr) {
        const tls_session copy(SSL_SESSION_dup(previous), &SSL_SESSION_free);
        REQUIRE(copy && SSL_set_session(stream.native_handle(), copy.get()) == 1);
    }
    if (!server_name.empty()) {
        REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), std::string(server_name).c_str()) == 1);
    }
    struct received_messages final {
        int alert_{-1};
        bool certificate_{};
    } received;
    SSL_set_msg_callback(stream.native_handle(),
        [](int writing, int, int type, const void* bytes, std::size_t size, SSL*, void* argument) {
            auto& result = *static_cast<received_messages*>(argument);
            if (!writing && type == SSL3_RT_ALERT && size == 2) {
                const auto* message = static_cast<const unsigned char*>(bytes);
                result.alert_ = message[0] == SSL3_AL_FATAL ? message[1] : -2;
            }
            if (!writing && type == SSL3_RT_HANDSHAKE && size > 0 &&
                *static_cast<const unsigned char*>(bytes) == SSL3_MT_CERTIFICATE) {
                result.certificate_ = true;
            }
        });
    SSL_set_msg_callback_arg(stream.native_handle(), &received);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    std::error_code error;
    stream.handshake(asio::ssl::stream_base::client, error);
    REQUIRE(error);
    REQUIRE(received.alert_ == expected_alert);
    REQUIRE(!received.certificate_);
    if (previous == nullptr) {
        const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(
            SSL_get1_peer_certificate(stream.native_handle()), &X509_free);
        REQUIRE(!certificate);
    }
}

// Writes raw request bytes in separate pieces, so the edge observes partial input,
// then returns every response byte until the edge closes the connection.
std::string exchange_raw(std::uint16_t port, std::initializer_list<std::string> pieces) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    for (const auto& piece : pieces) {
        asio::write(socket, asio::buffer(piece));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string response;
    std::array<char, 16384> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (error == asio::error::eof) {
            return response;
        }
        if (error) {
            throw std::system_error(error, "could not read edge response");
        }
    }
}

std::string response_body(std::string_view response) {
    const auto offset = response.find("\r\n\r\n");
    return offset == std::string_view::npos ? std::string{}
                                            : std::string(response.substr(offset + 4));
}

// Connects from a chosen local address, so one host acts as several clients.
asio::ip::tcp::socket connect_from(asio::io_context& context, std::string_view source,
    std::string_view target, std::uint16_t port) {
    const auto source_address = asio::ip::make_address(std::string(source));
    asio::ip::tcp::socket socket(context);
    socket.open(source_address.is_v4() ? asio::ip::tcp::v4() : asio::ip::tcp::v6());
    socket.bind({source_address, 0});
    socket.connect({asio::ip::make_address(std::string(target)), port});
    return socket;
}

std::string read_until_closed(asio::ip::tcp::socket& socket) {
    std::string bytes;
    std::array<char, 4096> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        bytes.append(buffer.data(), size);
        if (error == asio::error::eof) {
            return bytes;
        }
        if (error) {
            throw std::system_error(error, "could not read edge response");
        }
    }
}

void append_varint(std::string& output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

std::uint64_t read_varint(std::string_view bytes, std::size_t& offset) {
    std::uint64_t value{};
    for (unsigned shift = 0; shift < 64; shift += 7) {
        REQUIRE(offset < bytes.size());
        const auto byte = static_cast<unsigned char>(bytes[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            return value;
        }
    }
    throw std::runtime_error("invalid protobuf varint");
}

std::string length_delimited(std::uint32_t field, std::string_view bytes) {
    std::string result;
    append_varint(result, (static_cast<std::uint64_t>(field) << 3) | 2);
    append_varint(result, bytes.size());
    result.append(bytes);
    return result;
}

// Deterministic Website bytes as published before http2_enabled was retired: fields are in
// number order, so field 14 (true) precedes every field numbered above it.
std::string with_legacy_http2_field(std::string_view website) {
    std::size_t offset{};
    while (offset < website.size()) {
        const auto field_start = offset;
        const auto tag = read_varint(website, offset);
        if ((tag >> 3) > 14) {
            offset = field_start;
            break;
        }
        switch (tag & 7) {
            case 0:
                (void)read_varint(website, offset);
                break;
            case 1:
                offset += 8;
                break;
            case 2: {
                const auto length = read_varint(website, offset);
                offset += static_cast<std::size_t>(length);
                break;
            }
            case 5:
                offset += 4;
                break;
            default:
                throw std::runtime_error("unsupported protobuf wire type");
        }
    }
    // A later field must follow; otherwise appending would not exercise field ordering.
    REQUIRE(offset < website.size());
    return std::string(website.substr(0, offset)) + std::string("\x70\x01", 2) +
           std::string(website.substr(offset));
}

std::string web_socket_round_trip(std::uint16_t port) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    constexpr std::string_view handshake =
        "GET /ws HTTP/1.1\r\n"
        "Host: www.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(socket, asio::buffer(handshake));
    std::string response;
    std::array<char, 1024> buffer{};
    while (response.find("\r\n\r\n") == std::string::npos) {
        const auto size = socket.read_some(asio::buffer(buffer));
        response.append(buffer.data(), size);
    }
    REQUIRE(response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    constexpr std::string_view client_bytes = "PING";
    asio::write(socket, asio::buffer(client_bytes));
    std::array<char, 4> server_bytes{};
    asio::read(socket, asio::buffer(server_bytes));
    return std::string(server_bytes.data(), server_bytes.size());
}

enum class reload_action { abandon,
    fail,
    activate };

ruvia::task<void> hold_listener_reload(flexedge::node::data_plane& plane, const TestConfig& config,
    std::atomic<bool>& ready, ruvia::channel_receiver<bool>& release) {
    auto prepared = co_await flexedge::node::config_activation::prepare(plane, compiled(config));
    ready = true;
    REQUIRE((co_await release.receive_for(std::chrono::seconds(3))).has_value());
}

ruvia::task<void> exercise_listener_reload(flexedge::node::data_plane& plane,
    const TestConfig& config, std::size_t added, std::size_t retired, reload_action action) {
    std::optional<flexedge::node::config_activation> reload;
    {
        auto prepared = co_await flexedge::node::config_activation::prepare(plane, compiled(config));
        REQUIRE(prepared.added_listeners() == added);
        REQUIRE(prepared.retired_listeners() == retired);
        reload.emplace(std::move(prepared));
    }
    // The moved-from object's destruction must leave the new owner able to commit.
    if (action == reload_action::activate) {
        co_await reload->commit();
    } else if (action == reload_action::fail) {
        throw std::runtime_error("synthetic failure after preparing listeners");
    }
    co_return;
}

void verify_listener_reload() {
    using flexedge::node::data_plane;
    using flexedge::node::loop_shutdown;
    using flexedge::node::owned_log_batch;
    using flexedge::node::runtime_state;
    constexpr std::size_t worker_count = 2;
    ruvia::event_loop_pool workers({.loop_count_ = worker_count});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    runtime_state runtime;
    auto [sender, receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 32});
    flexedge::testing::temporary_cache_directory cache_directory;
    data_plane plane(workers, control.loop(0), runtime, sender, cache_directory.path_);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();

    asio::io_context sockets;
    const auto reserve_port = [&] {
        return asio::ip::tcp::acceptor(sockets, {asio::ip::address_v4::any(), 0});
    };
    const auto released = [&](std::uint16_t port) {
        return waitUntil([&] {
            asio::ip::tcp::acceptor probe(sockets);
            std::error_code error;
            probe.open(asio::ip::tcp::v4(), error);
            if (!error) {
                probe.set_option(asio::socket_base::reuse_address(true), error);
            }
            if (!error) {
                probe.bind({asio::ip::address_v4::any(), port}, error);
            }
            return !error;
        },
            std::chrono::seconds(2));
    };
    auto http_reservation = reserve_port();
    auto https_reservation = reserve_port();
    auto spare_reservation = reserve_port();
    auto occupied = reserve_port();
    const auto http_port = http_reservation.local_endpoint().port();
    const auto https_port = https_reservation.local_endpoint().port();
    const auto spare_port = spare_reservation.local_endpoint().port();
    http_reservation.close();
    https_reservation.close();
    spare_reservation.close();

    auto config = snapshot(false);
    const auto identity = make_identity("www.example.com", 10);
    config.mutableCertificate(0)->set_certificate_chain_pem(identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(identity.private_key_);
    auto* endpoint = config.node_spec.mutable_endpoints(0);
    endpoint->set_http_port(http_port);
    endpoint->set_https_port(https_port);
    auto* duplicate = config.node_spec.add_endpoints();
    *duplicate = *endpoint;
    duplicate->set_id("endpoint-duplicate");
    duplicate->set_ip_address("127.0.0.2");
    config.finalize();
    const auto exercise = [&](const TestConfig& candidate, std::size_t added, std::size_t retired, reload_action action) {
        control.loop(0).start(exercise_listener_reload(plane, candidate, added, retired, action)).get();
    };
    const auto require_serving = [&](std::uint16_t secure_port) {
        REQUIRE(request(http_port, "unknown.example.com").starts_with("HTTP/1.1 421 "));
        REQUIRE(tls_request(secure_port, "www.example.com", "unknown.example.com").bytes_.starts_with("HTTP/1.1 421 "));
    };
    // Both protocols deduplicate wildcard binds on every worker. Abandonment before start
    // releases them; moving and committing the same plan keeps them serving afterwards.
    exercise(config, 2 * worker_count, 0, reload_action::abandon);
    REQUIRE(released(http_port));
    REQUIRE(released(https_port));
    exercise(config, 2 * worker_count, 0, reload_action::activate);
    const auto active = runtime.config();
    require_serving(https_port);

    // Concurrent reports share one receiver, and must not consume one another's replies.
    std::vector<ruvia::root_task<flexedge::node::data_plane_report>> reports;
    for (std::size_t count = 0; count != 12; ++count) {
        reports.push_back(control.loop(0).start(plane.report({})));
    }
    for (auto& report : reports) {
        REQUIRE(report.get().load_.has_value());
    }
    // Cancelling a queued report must remove its waiter and let later operations proceed.
    auto [release_sender, release_receiver] = ruvia::make_channel<bool>(control.loop(0).handle(), {.capacity_ = 1});
    std::atomic<bool> held{};
    auto holding = control.loop(0).start(hold_listener_reload(plane, config, held, release_receiver));
    REQUIRE(waitUntil([&] { return held.load(); }, std::chrono::seconds(1)));
    ruvia::stop_source cancelled;
    auto cancelled_report = control.loop(0).start(plane.report(cancelled.token()));
    auto waiting_report = control.loop(0).start(plane.report({}));
    control.loop(0).start([]() -> ruvia::task<void> { co_return; }()).get();
    cancelled.request_stop();
    REQUIRE(!cancelled_report.get().load_.has_value());
    REQUIRE(release_sender.send(true).accepted());
    holding.get();
    REQUIRE(waiting_report.get().load_.has_value());

    // Abandoning a plan containing only reused listeners must not stop the active plane.
    exercise(config, 0, 0, reload_action::abandon);
    REQUIRE(runtime.config() == active);
    require_serving(https_port);

    auto replacement = config;
    replacement.setGeneration(4);
    for (auto& address : *replacement.node_spec.mutable_endpoints()) {
        address.set_https_port(spare_port);
    }
    replacement.finalize();
    exercise(replacement, worker_count, worker_count, reload_action::abandon);
    REQUIRE(released(spare_port));
    bool failed = false;
    try {
        exercise(replacement, worker_count, worker_count, reload_action::fail);
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "synthetic failure after preparing listeners";
    }
    REQUIRE(failed);
    REQUIRE(released(spare_port));
    REQUIRE(runtime.config() == active);
    require_serving(https_port);

    // A later HTTPS bind failure must release the HTTP listeners already prepared.
    auto blocked = replacement;
    for (auto& address : *blocked.node_spec.mutable_endpoints()) {
        address.set_http_port(spare_port);
        address.set_https_port(occupied.local_endpoint().port());
    }
    blocked.finalize();
    failed = false;
    try {
        exercise(blocked, 0, 0, reload_action::activate);
    } catch (const std::runtime_error&) {
        failed = true;
    }
    REQUIRE(failed);
    REQUIRE(released(spare_port));
    REQUIRE(runtime.config() == active);
    require_serving(https_port);

    exercise(replacement, worker_count, worker_count, reload_action::activate);
    REQUIRE(released(https_port));
    require_serving(spare_port);
    replacement.setGeneration(5);
    replacement.mutableWebsite()->set_https_enabled(false);
    for (auto& domain : *replacement.mutableWebsite()->mutable_domains()) {
        domain.set_https_enabled(false);
    }
    replacement.finalize();
    exercise(replacement, 0, worker_count, reload_action::activate);
    REQUIRE(released(spare_port));
    REQUIRE(request(http_port, "unknown.example.com").starts_with("HTTP/1.1 421 "));
    replacement.setGeneration(6);
    replacement.node_spec.set_enabled(false);
    replacement.finalize();
    exercise(replacement, 0, worker_count, reload_action::activate);
    REQUIRE(released(http_port));
    plane.request_stop_accepting();
    bool stopped = false;
    try {
        exercise(replacement, 0, 0, reload_action::activate);
    } catch (const std::runtime_error& error) {
        stopped = std::string_view(error.what()) == "data plane is stopping";
    }
    REQUIRE(stopped);
    shutdown.stop_and_join();
}

// Replaces the origins `health` tracks on its loop, as data_plane::activate does.
void set_health_origins(ruvia::event_loop loop, flexedge::node::origin_health_supervisor& health,
    flexedge::node::origin_health_key_set keys) {
    loop.start([](flexedge::node::origin_health_supervisor& supervisor,
                   flexedge::node::origin_health_key_set origins) -> ruvia::task<void> {
            supervisor.activate(supervisor.prepare(origins, {}));
            co_return;
        }(health, std::move(keys)))
        .get();
}

// Copies are taken on the owner loop; request outcomes no longer need two posted turns.
std::shared_ptr<const flexedge::node::origin_health_snapshot> settled_health(ruvia::event_loop loop,
    flexedge::node::origin_health_supervisor& health) {
    return loop.start([](flexedge::node::origin_health_supervisor& local) -> ruvia::task<std::shared_ptr<const flexedge::node::origin_health_snapshot>> {
                   co_return local.snapshot();
               }(health))
        .get();
}

void record_health_failure(ruvia::event_loop loop, flexedge::node::origin_health_supervisor& health) {
    loop.start([](flexedge::node::origin_health_supervisor& local) -> ruvia::task<void> {
            local.target("website-1", "origin-1").failure(1);
            co_return;
        }(health))
        .get();
}

ruvia::task<void> publish_runtime(flexedge::node::runtime_state& local,
    std::shared_ptr<const flexedge::node::compiled_config> config) {
    local.publish_execution(std::make_shared<const flexedge::node::worker_execution_view>(
        std::move(config)));
    co_return;
}

void publish_tls(ruvia::event_loop loop, flexedge::node::tls_context_registry& registry,
    std::shared_ptr<const flexedge::node::compiled_config> config) {
    loop.start([](flexedge::node::tls_context_registry& local,
                   std::shared_ptr<const flexedge::node::compiled_config> material) -> ruvia::task<void> {
            local.publish(std::make_shared<flexedge::node::tls_context_set>(*material));
            co_return;
        }(registry, std::move(config)))
        .get();
}

struct observed_worker final {
    ruvia::event_loop loop_;
    flexedge::node::worker_metrics* metrics_{};
    const flexedge::node::connected_clients* clients_{};
    const flexedge::node::log_producer* logs_{};
};
struct observed_counters final {
    flexedge::node::data_plane_load load_{};
    std::vector<flexedge::node::connected_clients::address_key> addresses_{};
};
ruvia::task<observed_counters> capture_counters(const observed_worker& worker) {
    co_return observed_counters{.load_ = {.connection_count_ = worker.metrics_->connection_count(),
                                    .traffic_out_bytes_ = worker.metrics_->traffic_out_bytes(),
                                    .logs_ = worker.logs_->snapshot()},
        .addresses_ = worker.clients_->snapshot()};
}

}  // namespace

// This integration test owns one shared runtime and network lifecycle so each assertion observes
// the same state.
// NOLINTNEXTLINE(google-readability-function-size)
int main(int argc, char* argv[]) {
    try {
        if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
            throw std::runtime_error("test executable path is unavailable");
        }
        const auto executable_path = std::filesystem::absolute(argv[0]);
        REQUIRE(flexedge::node::valid_credential_node_id(std::string(32, 'a')));
        REQUIRE(!flexedge::node::valid_credential_node_id(std::string(32, 'A')));
        REQUIRE(!flexedge::node::valid_credential_node_id(std::string(31, 'a')));
        REQUIRE(!flexedge::node::valid_credential_node_id(std::string(33, 'a')));
        REQUIRE(flexedge::node::valid_credential_secret(std::string(32, '!')));
        REQUIRE(flexedge::node::valid_credential_secret(std::string(128, '~')));
        REQUIRE(!flexedge::node::valid_credential_secret(std::string(31, '!')));
        REQUIRE(!flexedge::node::valid_credential_secret(std::string(129, '!')));
        REQUIRE(!flexedge::node::valid_credential_secret(std::string(32, ' ')));
        REQUIRE(!flexedge::node::valid_credential_secret(std::string(32, '\x7f')));
        const auto credentials_root =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-credentials-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct credentials_directory_cleanup final {
            std::filesystem::path path_;
            ~credentials_directory_cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } credentials_cleanup{credentials_root};
        const auto credentials_path = credentials_root / "credentials";
        constexpr std::string_view node_id = "0123456789abcdef0123456789abcdef";
        constexpr std::string_view secret = "0123456789abcdefABCDEFghijklmnop";
        flexedge::node::write_secure_file_atomic(credentials_path,
            "node_id=0123456789abcdef0123456789abcdef\n"
            "secret=0123456789abcdefABCDEFghijklmnop\n");
        auto credentials = flexedge::node::node_credentials::load(credentials_path);
        REQUIRE(credentials.node_id() == node_id);
        REQUIRE(credentials.secret() == secret);
        REQUIRE(std::filesystem::exists(credentials_path));
        const auto invalid_credentials_path = credentials_root / "invalid-credentials";
        const std::string valid_credential_text =
            "node_id=" + std::string(node_id) + "\nsecret=" + std::string(secret) + "\n";
        for (const auto& prefix : {"node_id=\n", "secret=\n", "node_id=duplicate\n",
                 "secret=duplicate\n"}) {
            flexedge::node::write_secure_file_atomic(invalid_credentials_path,
                prefix + valid_credential_text);
            bool rejected = false;
            try {
                (void)flexedge::node::node_credentials::load(invalid_credentials_path);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            REQUIRE(rejected);
        }
#ifndef _WIN32
        const auto credential_permissions = std::filesystem::status(credentials_path).permissions();
        REQUIRE((credential_permissions & std::filesystem::perms::group_all) ==
                std::filesystem::perms::none);
        REQUIRE((credential_permissions & std::filesystem::perms::others_all) ==
                std::filesystem::perms::none);
#endif

        verify_log_batches();
        verify_listener_reload();

        const auto updater_binary_path = credentials_root / "node";
        {
            std::ofstream output(updater_binary_path, std::ios::binary | std::ios::trunc);
            output << "current-node";
        }
        const std::string next_node_bytes{"next-node-from-control-websocket"};
        const auto next_node_path = credentials_root / "node.next";
        {
            std::ofstream output(next_node_path, std::ios::binary | std::ios::trunc);
            output.write(next_node_bytes.data(), static_cast<std::streamsize>(next_node_bytes.size()));
        }
        const auto next_node_digest = flexedge::crypto::file_sha256(next_node_path);
        bool restart_requested{};
        flexedge::node::self_updater ws_updater({
            .binary_path_ = updater_binary_path,
            .current_version_ = "0.0.0",
            .upgrade_record_path_ = credentials_root / "pending-upgrade",
            .node_log_ = {},
            .request_restart_ = [&] { restart_requested = true; },
        });
        REQUIRE(!ws_updater.update_available("invalid"));
        REQUIRE(!ws_updater.update_available(flexedge::crypto::file_sha256(updater_binary_path)));
        REQUIRE(ws_updater.update_available(next_node_digest));
        const auto upgrade_rejected = [](auto&& action) {
            try {
                action();
                return false;
            } catch (const std::runtime_error&) {
                return true;
            }
        };
        const auto original_digest = flexedge::crypto::file_sha256(updater_binary_path);
        ws_updater.begin("0.0.1", next_node_digest,
            static_cast<std::uint64_t>(next_node_bytes.size()));
        REQUIRE(upgrade_rejected([&] { ws_updater.append(1, "bad-offset"); }));
        REQUIRE(upgrade_rejected([&] { ws_updater.append(0, ""); }));
        REQUIRE(upgrade_rejected([&] { ws_updater.append(0, std::string(next_node_bytes.size() + 1, 'x')); }));
        REQUIRE(upgrade_rejected([&] { ws_updater.commit(); }));
        REQUIRE(ws_updater.written_bytes() == 0);
        ws_updater.append(0, "partial");
        ws_updater.abort();
        REQUIRE(!restart_requested);
        REQUIRE(flexedge::crypto::file_sha256(updater_binary_path) == original_digest);
        ws_updater.begin("0.0.1", next_node_digest,
            static_cast<std::uint64_t>(next_node_bytes.size()));
        ws_updater.append(0, std::string(next_node_bytes.size(), 'x'));
        REQUIRE(upgrade_rejected([&] { ws_updater.commit(); }));
        REQUIRE(!restart_requested);
        REQUIRE(flexedge::crypto::file_sha256(updater_binary_path) == original_digest);
        for (const auto& entry : std::filesystem::directory_iterator(credentials_root)) {
            REQUIRE(!entry.path().filename().string().starts_with(".node.upgrade."));
        }
        ws_updater.begin("0.0.1", next_node_digest,
            static_cast<std::uint64_t>(next_node_bytes.size()));
        ws_updater.append(0, std::string_view(next_node_bytes).substr(0, 9));
        REQUIRE(!ws_updater.complete());
        REQUIRE(ws_updater.written_bytes() == 9);
        ws_updater.append(9, std::string_view(next_node_bytes).substr(9));
        REQUIRE(ws_updater.complete());
#ifndef _WIN32
        ws_updater.commit();
        REQUIRE(restart_requested);
        REQUIRE(flexedge::crypto::file_sha256(updater_binary_path) == next_node_digest);
        const auto ws_upgrade = flexedge::node::take_pending_upgrade_record(
            credentials_root / "pending-upgrade", next_node_digest);
        REQUIRE(ws_upgrade && ws_upgrade->current_version_ == "0.0.1");
#else
        REQUIRE(upgrade_rejected([&] { ws_updater.commit(); }));
        REQUIRE(!flexedge::node::take_pending_upgrade_record(
            credentials_root / "pending-upgrade", original_digest));
        REQUIRE(!std::filesystem::exists(credentials_root / "pending-upgrade"));
        REQUIRE(!restart_requested);
        REQUIRE(flexedge::crypto::file_sha256(updater_binary_path) == original_digest);
        for (const auto& entry : std::filesystem::directory_iterator(credentials_root)) {
            REQUIRE(!entry.path().filename().string().starts_with(".node.upgrade."));
        }
#endif

        REQUIRE(flexedge::node::accepts_event_stream("text/event-stream"));
        REQUIRE(flexedge::node::accepts_event_stream(
            "application/json, text/event-stream; charset=utf-8"));
        REQUIRE(!flexedge::node::accepts_event_stream("text/plain"));
        const std::array<ruvia::http_header_view, 2> streaming_headers{
            ruvia::http_header_view{"Accept", "text/event-stream"},
            ruvia::http_header_view{"Accept-Encoding", "gzip"},
        };
        const auto streaming_config = snapshot();
        const auto streaming_origin_request = flexedge::node::prepare_origin_request(
            {.method_ = "GET",
                .target_ = "/events",
                .headers_ = streaming_headers,
                .body_ = {},
                .has_body_ = false},
            streaming_config.website(), "www.example.com", "198.51.100.1", true, false, false, true);
        REQUIRE(streaming_origin_request);
        REQUIRE(streaming_origin_request->bytes_.find("Accept-Encoding: identity\r\n") !=
                std::string::npos);
        REQUIRE(streaming_origin_request->bytes_.find("Accept-Encoding: gzip\r\n") ==
                std::string::npos);
        auto origin_exchange_state = [&](std::string_view method = "GET") {
            auto prepared = flexedge::node::prepare_origin_request(
                {.method_ = method, .target_ = "/", .headers_ = {}, .body_ = {}, .has_body_ = false},
                streaming_config.website(), "www.example.com", "198.51.100.1", true, false, false);
            REQUIRE(prepared);
            return prepared->make_exchange_state();
        };
        // Streaming keeps the origin framing for the relayed body and only tracks its end.
        flexedge::node::origin_response_codec streamed(origin_exchange_state(), true, nullptr);
        REQUIRE(streamed.consume_head("HTTP/1.1 200 OK\r\n"
                                      "Connection: keep-alive, X-Remove\r\n"
                                      "X-Remove: secret\r\n"
                                      "Keep-Alive: timeout=5\r\n"
                                      "Transfer-Encoding: chunked\r\n"
                                      "Strict-Transport-Security: max-age=1\r\n\r\n"
                                      "4\r\nWi") == flexedge::node::origin_response_head_status::ready);
        const auto streamed_head = streamed.take_output();
        REQUIRE(streamed_head.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(streamed_head.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
        REQUIRE(streamed_head.find("X-Remove") == std::string::npos);
        REQUIRE(streamed_head.find("Keep-Alive") == std::string::npos);
        REQUIRE(streamed_head.find("max-age=1\r\n") == std::string::npos);
        REQUIRE(streamed_head.find("Strict-Transport-Security: max-age=31536000\r\n") !=
                std::string::npos);
        REQUIRE(streamed_head.ends_with(
            "Connection: close\r\n"
            "Strict-Transport-Security: max-age=31536000\r\n\r\n4\r\nWi"));
        REQUIRE(!streamed.complete());
        REQUIRE(streamed.consume_body("ki\r\n0\r\nX-Test: value\r\n") ==
                flexedge::node::origin_response_body_status::accepted);
        REQUIRE(!streamed.complete());
        REQUIRE(streamed.consume_body("\r\n") == flexedge::node::origin_response_body_status::accepted);
        REQUIRE(streamed.complete());
        REQUIRE(streamed.consume_body("extra") == flexedge::node::origin_response_body_status::failure);

        // The streaming codec preserves representation length on HEAD/304 and
        // never invents Content-Length for 204, including with compression enabled.
        struct no_content_response final {
            std::string_view method_{};
            std::string_view head_{};
            std::uint16_t status_{};
        };
        flexedge::node::buffered_bytes_budget codec_budget(1024 * 1024);
        auto codec_website = streaming_config.website();
        codec_website.set_response_compression_enabled(true);
        for (const bool compression : {false, true}) {
            for (const auto& test : std::array{
                     no_content_response{"HEAD", "HTTP/1.1 200 OK\r\nContent-Length: 123\r\n\r\n", 200},
                     no_content_response{"GET", "HTTP/1.1 304 Not Modified\r\nContent-Length: 123\r\n\r\n", 304},
                     no_content_response{"GET", "HTTP/1.1 204 No Content\r\n\r\n", 204}}) {
                flexedge::node::origin_response_codec codec(origin_exchange_state(test.method_), false, nullptr);
                if (compression) {
                    codec.set_compression(codec_website, "/file.html", "gzip", codec_budget.lease());
                }
                REQUIRE(codec.consume_head(test.head_) == flexedge::node::origin_response_head_status::ready);
                REQUIRE(codec.complete());
                const auto wire = codec.take_output();
                ruvia::http1_client_response_parser parser(origin_exchange_state(test.method_));
                const auto result = parser.parse(wire);
                REQUIRE(result.parsed());
                REQUIRE(result.parsed()->head().status().value() == test.status_);
                REQUIRE(result.parsed()->consumed_bytes() == wire.size());
                for (const auto& field : result.parsed()->head().headers()) {
                    if (flexedge::route_policy::ascii_equal(field.name(), "Content-Length")) {
                        REQUIRE(test.status_ != 204);
                        REQUIRE(field.value() == "123");
                    }
                }
                REQUIRE(wire.ends_with("\r\n\r\n"));
                REQUIRE(codec_budget.used_bytes() == 0);
            }
        }

        flexedge::node::origin_response_codec malformed(origin_exchange_state(), false, nullptr);
        REQUIRE(malformed.consume_head("HTTP/1.1 20x Bad Status\r\n\r\n") ==
                flexedge::node::origin_response_head_status::failure);

        flexedge::node::buffered_bytes_budget budget(8192);
        auto first_reservation = budget.lease();
        REQUIRE(first_reservation.try_grow(4096));
        REQUIRE(budget.used_bytes() == 4096);
        auto moved_reservation = std::move(first_reservation);
        moved_reservation.shrink(1024);
        REQUIRE(budget.used_bytes() == 3072);
        moved_reservation = {};
        REQUIRE(budget.used_bytes() == 0);
        auto maximum_reservation = budget.lease();
        REQUIRE(maximum_reservation.try_grow(budget.maximum_bytes()));
        auto rejected_reservation = budget.lease();
        REQUIRE(!rejected_reservation.try_grow(1));
        REQUIRE(budget.rejected_reservations() == 1);
        maximum_reservation = {};
        REQUIRE(budget.used_bytes() == 0);

        flexedge::node::buffered_bytes_budget request_budget(32);
        auto held_request_bytes = request_budget.lease();
        REQUIRE(held_request_bytes.try_grow(32));
        auto rejected_request_bytes = request_budget.lease();
        REQUIRE(!rejected_request_bytes.try_grow(1));
        REQUIRE(request_budget.rejected_reservations() == 1);

        flexedge::node::state_cipher state_cipher(std::string(64, '1'));
        const auto sealed_state = state_cipher.seal("device-encrypted-private-state");
        REQUIRE(sealed_state.starts_with("FES1"));
        REQUIRE(sealed_state.find("device-encrypted-private-state") == std::string::npos);
        REQUIRE(state_cipher.open(sealed_state) == "device-encrypted-private-state");
        auto tampered_state = sealed_state;
        tampered_state.back() ^= 1;
        bool tampered_state_rejected = false;
        try {
            (void)state_cipher.open(tampered_state);
        } catch (const std::runtime_error&) {
            tampered_state_rejected = true;
        }
        REQUIRE(tampered_state_rejected);

        const auto state_directory =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-state-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct state_directory_cleanup final {
            std::filesystem::path path_;
            ~state_directory_cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } state_cleanup{state_directory};
        const auto node_digest = flexedge::crypto::file_sha256(executable_path);
        const auto upgrade_record_path = state_directory / "pending-upgrade";
        const flexedge::node::upgrade_record upgrade_record{
            .previous_version_ = "0.3.23",
            .current_version_ = "0.3.24",
            .target_sha256_ = node_digest,
        };
        flexedge::node::write_pending_upgrade_record(upgrade_record_path, upgrade_record);
        const auto restored_upgrade =
            flexedge::node::take_pending_upgrade_record(upgrade_record_path, node_digest);
        REQUIRE(restored_upgrade.has_value());
        REQUIRE(restored_upgrade->message() == upgrade_record.message());
        REQUIRE(!std::filesystem::exists(upgrade_record_path));
        flexedge::node::write_pending_upgrade_record(upgrade_record_path, upgrade_record);
        REQUIRE(!flexedge::node::take_pending_upgrade_record(
            upgrade_record_path, "e1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea"));
        REQUIRE(!std::filesystem::exists(upgrade_record_path));
        flexedge::node::state_store state_store(state_directory,
            "3df64f75-2df7-43b8-aa89-661920126564");
        auto stored_snapshot = snapshot();
        stored_snapshot.finalize();
        state_store.stage(stored_snapshot.active, stored_snapshot.objects);
        REQUIRE(!std::filesystem::exists(state_directory / "releases"));
        const auto no_previous = state_store.activate_staged();
        REQUIRE(!no_previous);
        const auto persisted = state_store.load();
        REQUIRE(persisted.active_.SerializeAsString() == stored_snapshot.active.SerializeAsString());
        REQUIRE(persisted.objects_.size() == stored_snapshot.objects.size());
        state_store.rollback_active(no_previous);
        REQUIRE(!state_store.load().active_.has_release());
        state_store.stage(stored_snapshot.active, stored_snapshot.objects);
        state_store.activate_staged();
        auto rolled_back = stored_snapshot;
        rolled_back.setGeneration(4);
        rolled_back.finalize();
        state_store.stage(rolled_back.active, rolled_back.objects);
        const auto previous_state = state_store.activate_staged();
        REQUIRE(previous_state.has_value());
        REQUIRE(state_store.load().active_.SerializeAsString() == rolled_back.active.SerializeAsString());
        state_store.rollback_active(previous_state);
        REQUIRE(state_store.load().active_.SerializeAsString() == stored_snapshot.active.SerializeAsString());
        auto stale_snapshot = snapshot();
        stale_snapshot.node_spec.set_schema_version(1);
        stale_snapshot.finalize();
        state_store.stage(stale_snapshot.active, stale_snapshot.objects);
        state_store.activate_staged();
        const auto discarded = state_store.load();
        REQUIRE(!discarded.active_.has_node_spec());
        REQUIRE(discarded.objects_.empty());
        auto stale_release = snapshot();
        stale_release.release.set_schema_version(1);
        stale_release.finalize();
        state_store.stage(stale_release.active, stale_release.objects);
        state_store.activate_staged();
        const auto discarded_release = state_store.load();
        REQUIRE(!discarded_release.active_.has_release());
        REQUIRE(discarded_release.objects_.empty());
        for (const auto version : {2u, 3u, 4u}) {
            auto compatible = snapshot();
            compatible.release.set_schema_version(version);
            compatible.finalize();
            state_store.stage(compatible.active, compatible.objects);
            state_store.activate_staged();
            REQUIRE(state_store.load().active_.has_release());
        }

        const auto config = compiled(snapshot());
        REQUIRE(config->website("www.example.com:80") != nullptr);
        REQUIRE(config->domain("WWW.EXAMPLE.COM") != nullptr);
        // The port goes before the trailing root label dot: both spellings name the same site.
        REQUIRE(flexedge::route_policy::hostname("WWW.Example.COM.:443") == "www.example.com");
        REQUIRE(config->website("WWW.Example.COM.:443") == config->website("www.example.com"));
        REQUIRE(config->domain("www.example.com.") != nullptr);
        const auto redirect =
            flexedge::node::http_to_https_redirect(*config, "WWW.Example.COM:80", "/a?q=1");
        REQUIRE(redirect);
        REQUIRE(redirect->status_ == ruvia::http_status::moved_permanently);
        REQUIRE(redirect->location_ == "https://www.example.com/a?q=1");
        REQUIRE(!flexedge::node::http_to_https_redirect(*config, "http.example.com", "/"));

        const auto no_redirect = compiled(snapshot(false));
        REQUIRE(!flexedge::node::http_to_https_redirect(*no_redirect, "www.example.com", "/"));

        auto wildcard_snapshot = snapshot();
        wildcard_snapshot.mutableWebsite()->mutable_domains(0)->set_hostname("*.Example.COM.");
        wildcard_snapshot.duplicateWebsite();
        auto* exact_website = wildcard_snapshot.contents.back().mutable_website();
        exact_website->set_id("exact-website");
        exact_website->clear_domains();
        auto* exact_domain = exact_website->add_domains();
        exact_domain->set_hostname("www.example.com");
        exact_domain->set_https_enabled(true);
        exact_domain->set_certificate_digest(
            wildcard_snapshot.mutableWebsite()->domains(0).certificate_digest());
        const auto wildcard_config = compiled(std::move(wildcard_snapshot));
        REQUIRE(wildcard_config->website("www.example.com")->id() == "exact-website");
        REQUIRE(wildcard_config->website("ASSETS.Example.COM.:443")->id() == "website-1");
        REQUIRE(wildcard_config->domain("assets.example.com")->hostname() == "*.Example.COM.");
        REQUIRE(!wildcard_config->domain("http.example.com")->https_enabled());
        for (const auto host : {"example.com", "a.b.example.com", "badexample.com",
                 "example.com.evil", "*.example.com", ".example.com", "bad_host.example.com"}) {
            REQUIRE(wildcard_config->website(host) == nullptr);
            REQUIRE(wildcard_config->domain(host) == nullptr);
        }
        const auto wildcard_redirect = flexedge::node::http_to_https_redirect(
            *wildcard_config, "ASSETS.Example.COM:80", "/asset?q=1");
        REQUIRE(wildcard_redirect);
        REQUIRE(wildcard_redirect->location_ == "https://assets.example.com/asset?q=1");

        // Objects published before http2_enabled was retired carry field 14 in number order.
        // Nodes hash the bytes they hold, so those objects verify as published; nodes that
        // still re-serialize need the field known to reproduce the same bytes.
        {
            const auto legacy_website = with_legacy_http2_field(
                flexedge::node::serialize_artifact(snapshot(false).website()));
            const auto legacy_content = length_delimited(10, legacy_website);
            const auto legacy_digest = flexedge::node::artifact_digest(std::string_view(legacy_content));

            // The object file exactly as the previous node binary persisted it.
            constexpr std::string_view legacy_secret = "3df64f75-2df7-43b8-aa89-661920126564";
            const auto legacy_directory = state_directory / "legacy-http2";
            flexedge::node::write_secure_file_atomic(
                legacy_directory / "objects" / (legacy_digest + ".pb"),
                flexedge::node::state_cipher(legacy_secret)
                    .seal(length_delimited(1, legacy_digest) + length_delimited(2, legacy_content)));
            const flexedge::node::state_store legacy_store(legacy_directory, legacy_secret);
            const auto legacy_object = legacy_store.load_object(legacy_digest);
            REQUIRE(legacy_object && legacy_object->content() == legacy_content);
            const auto parsed_content =
                flexedge::node::open_artifact<flexedge::node::v2::DeliveryObjectContent>(
                    legacy_digest, legacy_object->content());
            REQUIRE(parsed_content && parsed_content->has_website());
            REQUIRE(flexedge::node::serialize_artifact(*parsed_content) == legacy_content);

            auto legacy_http2 = snapshot(false);
            *legacy_http2.mutableWebsite() = parsed_content->website();
            const auto legacy_http2_config = compiled(std::move(legacy_http2));
            REQUIRE(legacy_http2_config->website("www.example.com") != nullptr);
        }

        auto invalid_compression_mime_type = snapshot(false);
        invalid_compression_mime_type.mutableWebsite()->add_response_compression_mime_types(
            "application");
        bool invalid_compression_mime_type_rejected = false;
        try {
            (void)compiled(std::move(invalid_compression_mime_type));
        } catch (const std::runtime_error&) {
            invalid_compression_mime_type_rejected = true;
        }
        REQUIRE(invalid_compression_mime_type_rejected);

        flexedge::node::runtime_state runtime;
        apply(runtime, snapshot());
        const auto previous = runtime.config();
        REQUIRE(previous != nullptr);
        REQUIRE(previous->node_spec().revision() == 3);

        auto duplicate = snapshot();
        duplicate.setGeneration(4);
        duplicate.duplicateWebsite();
        bool rejected = false;
        try {
            apply(runtime, std::move(duplicate));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        REQUIRE(rejected);
        REQUIRE(runtime.config() == previous);
        REQUIRE(runtime.applied_node_spec_revision() == 3);

        auto replacement = snapshot(false);
        replacement.setGeneration(4);
        apply(runtime, std::move(replacement));
        REQUIRE(runtime.config() != previous);
        REQUIRE(runtime.applied_node_spec_revision() == 4);
        REQUIRE(previous->website("www.example.com") != nullptr);
        std::int64_t next_generation{5};

        // Declared before the loops so sessions retiring on them never outlive their counters.
        flexedge::node::connected_clients first_clients;
        flexedge::node::connected_clients second_clients;
        flexedge::node::worker_metrics first_metrics;
        flexedge::node::worker_metrics second_metrics;
        ruvia::event_loop_pool loops({.loop_count_ = 2});
        ruvia::event_loop_pool control_loops({.loop_count_ = 1});
        flexedge::node::runtime_state first_runtime;
        flexedge::node::runtime_state second_runtime;
        first_runtime.publish_execution(std::make_shared<const flexedge::node::worker_execution_view>(
            runtime.config()));
        second_runtime.publish_execution(std::make_shared<const flexedge::node::worker_execution_view>(
            runtime.config()));
        flexedge::node::origin_tls_context first_origin_tls;
        flexedge::node::origin_tls_context second_origin_tls;
        auto listener_health =
            flexedge::node::origin_health_supervisor::create(loops.loop(0), first_runtime, first_origin_tls);
        auto second_listener_health = flexedge::node::origin_health_supervisor::create(loops.loop(1), second_runtime, second_origin_tls);
        flexedge::node::origin_connection_pool first_origin_connections(loops.loop(0).executor(),
            first_origin_tls);
        flexedge::node::origin_connection_pool second_origin_connections(loops.loop(1).executor(),
            second_origin_tls);
        flexedge::node::buffered_bytes_budget request_buffers(
            flexedge::node::maximum_request_buffer_bytes);
        flexedge::node::buffered_bytes_budget response_buffers(
            flexedge::node::maximum_response_buffer_bytes);
        flexedge::node::buffered_bytes_budget second_request_buffers(flexedge::node::maximum_request_buffer_bytes);
        flexedge::node::buffered_bytes_budget second_response_buffers(flexedge::node::maximum_response_buffer_bytes);
        auto [log_sender, log_receiver] =
            ruvia::make_channel<flexedge::node::owned_log_batch>(control_loops.loop(0).handle(), {.capacity_ = 128});
        flexedge::node::log_producer first_worker_logs(loops.loop(0), log_sender);
        flexedge::node::log_producer second_worker_logs(loops.loop(1), log_sender);
        flexedge::node::runtime_metrics_sampler metrics_sampler;
        (void)metrics_sampler.sample(std::uint64_t{0});
        const auto session_context = [&](flexedge::node::origin_health_supervisor& health,
                                         flexedge::node::worker_metrics& metrics,
                                         flexedge::node::connected_clients& clients,
                                         flexedge::node::log_producer& logs,
                                         flexedge::node::origin_connection_pool& origin_connections,
                                         flexedge::node::buffered_bytes_budget& requests) {
            return flexedge::node::http_session_context{
                .runtime_ = first_runtime,
                .health_ = health,
                .metrics_ = metrics,
                .clients_ = clients,
                .logs_ = logs,
                .origin_connections_ = origin_connections,
                .request_buffers_ = requests,
                .response_buffers_ = response_buffers,
            };
        };
        const auto first_context = session_context(*listener_health, first_metrics, first_clients,
            first_worker_logs, first_origin_connections, request_buffers);
        const flexedge::node::http_session_context second_context{
            .runtime_ = second_runtime, .health_ = *second_listener_health, .metrics_ = second_metrics, .clients_ = second_clients, .logs_ = second_worker_logs, .origin_connections_ = second_origin_connections, .request_buffers_ = second_request_buffers, .response_buffers_ = second_response_buffers};
        // As data_plane does once the configuration that added a listener is active.
        const auto start = [](const std::shared_ptr<flexedge::node::edge_listener>& listener) {
            listener->request_start();
            listener->activate();
        };
        auto first_listener = flexedge::node::edge_listener::create(loops.loop(0), first_context,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
        const auto port = first_listener->local_endpoint().port();
        auto second_listener = flexedge::node::edge_listener::create(loops.loop(1), second_context,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port), nullptr);
        flexedge::node::buffered_bytes_budget constrained_request_buffers(32);
        auto constrained_listener = flexedge::node::edge_listener::create(loops.loop(0),
            session_context(*listener_health, first_metrics, first_clients, first_worker_logs,
                first_origin_connections, constrained_request_buffers),
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
        const auto constrained_port = constrained_listener->local_endpoint().port();
        start(first_listener);
        start(second_listener);
        start(constrained_listener);
        flexedge::node::loop_shutdown shutdown(loops, control_loops);
        loops.start();
        control_loops.start();
        const auto publish = [&](TestConfig value) {
            apply(runtime, std::move(value));
            const auto config = runtime.config();
            loops.loop(0).start(publish_runtime(first_runtime, config)).get();
            loops.loop(1).start(publish_runtime(second_runtime, config)).get();
        };
        const auto collect_node_logs = [&] {
            loops.loop(0).start(flush_logs(first_worker_logs)).get();
            loops.loop(1).start(flush_logs(second_worker_logs)).get();
            return control_loops.loop(0).start(receive_node_logs(log_receiver)).get();
        };
        // Each worker owns its counters; a separate control loop collects their immutable
        // copies, as the heartbeat does.
        const std::array connected_workers{
            observed_worker{
                .loop_ = loops.loop(0), .metrics_ = &first_metrics, .clients_ = &first_clients, .logs_ = &first_worker_logs},
            observed_worker{
                .loop_ = loops.loop(1), .metrics_ = &second_metrics, .clients_ = &second_clients, .logs_ = &second_worker_logs},
        };
        const auto load_of = [&](std::span<const observed_worker> workers) -> std::optional<flexedge::node::data_plane_load> {
            flexedge::node::data_plane_load result;
            std::vector<flexedge::node::connected_clients::address_key> addresses;
            for (const auto& worker : workers) {
                const auto copy = worker.loop_.start(capture_counters(worker)).get();
                result.connection_count_ += copy.load_.connection_count_;
                result.traffic_out_bytes_ += copy.load_.traffic_out_bytes_;
                result.logs_.published_events_ += copy.load_.logs_.published_events_;
                result.logs_.queued_events_ += copy.load_.logs_.queued_events_;
                result.logs_.dropped_events_ += copy.load_.logs_.dropped_events_;
                addresses.insert(addresses.end(), copy.addresses_.begin(), copy.addresses_.end());
            }
            std::ranges::sort(addresses);
            addresses.erase(std::ranges::unique(addresses).begin(), addresses.end());
            result.connected_ip_count_ = static_cast<std::int64_t>(addresses.size());
            return result;
        };
        const auto count_ips =
            [&](std::span<const observed_worker> workers) -> std::optional<std::int64_t> {
            if (const auto load = load_of(workers)) {
                return load->connected_ip_count_;
            }
            return std::nullopt;
        };
        const auto connected_ips_reach = [&](std::int64_t expected) {
            return waitUntil([&] { return count_ips(connected_workers) == expected; },
                std::chrono::seconds(2));
        };
        const auto connections_reach = [&](std::int64_t expected) {
            return waitUntil(
                [&] {
                    const auto load = load_of(connected_workers);
                    return load && load->connection_count_ == expected;
                },
                std::chrono::seconds(2));
        };
        // A listener stopped before its configuration activates must release its
        // self references even if another start request was queued.
        auto verify_stopped_before_activation = [&](auto listener) {
            const std::weak_ptr weak(listener);
            listener->request_start();
            listener->request_start();
            listener->request_stop();
            listener.reset();
            REQUIRE(waitUntil([&] { return weak.expired(); }, std::chrono::seconds(2)));
        };
        verify_stopped_before_activation(flexedge::node::edge_listener::create(loops.loop(0),
            first_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr));
        flexedge::node::tls_context_registry inactive_tls_contexts;
        verify_stopped_before_activation(flexedge::node::edge_listener::create(loops.loop(0),
            first_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            &inactive_tls_contexts));
        const auto constrained_response = request(constrained_port);
        REQUIRE(constrained_response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
        REQUIRE(waitUntil([&] { return constrained_request_buffers.used_bytes() == 0; },
            std::chrono::seconds(1)));
        REQUIRE(constrained_request_buffers.rejected_reservations() == 1);
        constrained_listener->request_stop();
        // Retire and recreate the health record only after a real upstream
        // request arrives. Its subsequent failure belongs to the old target.
        {
            asio::io_context origin_context;
            asio::ip::tcp::acceptor origin_acceptor(origin_context,
                {asio::ip::address_v4::loopback(), 0});
            auto pending_config = snapshot(false);
            pending_config.setGeneration(next_generation++);
            pending_config.mutableWebsite()->set_unhealthy_threshold(1);
            auto* origin = pending_config.mutableWebsite()->mutable_origins(0);
            origin->set_host("127.0.0.1");
            origin->set_port(origin_acceptor.local_endpoint().port());
            publish(std::move(pending_config));
            const flexedge::node::origin_health_key_set keys{
                {.website_id_ = "website-1", .origin_id_ = "origin-1"}};
            set_health_origins(loops.loop(0), *listener_health, keys);
            set_health_origins(loops.loop(1), *second_listener_health, keys);
            std::atomic<bool> replaced_health{false};
            std::jthread upstream([&](std::stop_token stop) {
                asio::ip::tcp::socket socket(origin_context);
                if (!accept_until_stopped(origin_acceptor, socket, stop)) {
                    return;
                }
                std::array<char, 4096> bytes{};
                std::error_code error;
                if (socket.read_some(asio::buffer(bytes), error) == 0 || error) {
                    return;
                }
                set_health_origins(loops.loop(0), *listener_health, {});
                set_health_origins(loops.loop(1), *second_listener_health, {});
                set_health_origins(loops.loop(0), *listener_health, keys);
                set_health_origins(loops.loop(1), *second_listener_health, keys);
                replaced_health.store(true);
                // Closing without a response drives the real failure callback.
            });
            const auto failed_response = request(port);
            upstream.request_stop();
            upstream.join();
            REQUIRE(replaced_health.load());
            REQUIRE(failed_response.starts_with("HTTP/1.1 502 Bad Gateway\r\n"));
            REQUIRE(settled_health(loops.loop(0), *listener_health)->healthy({"website-1", "origin-1"}));
            REQUIRE(settled_health(loops.loop(1), *second_listener_health)->healthy({"website-1", "origin-1"}));
        }
        const auto no_route_response = request(port, "unknown.example.com");
        REQUIRE(no_route_response.starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        REQUIRE(exchange_raw(port, {"GET / HTTP/1.1\r\nConnection: close\r\n\r\n"}).starts_with("HTTP/1.1 400 "));
        REQUIRE(request(port, "www.example.com", "Host: unknown.example.com\r\n").starts_with("HTTP/1.1 400 "));
        REQUIRE(connections_reach(0));
        (void)collect_node_logs();
        // Invalid incoming Host values are request errors, not broken node configuration.
        for (const auto& host : {std::string("."), std::string(254, 'a')}) {
            REQUIRE(request(port, host).starts_with("HTTP/1.1 400 Bad Request\r\n"));
            REQUIRE(connections_reach(0));
            const auto entries = collect_node_logs();
            REQUIRE(entries.size() == 1);
            REQUIRE(entries.front().level() == "warning");
            REQUIRE(entries.front().category() == "http1");
            REQUIRE(entries.front().message().starts_with("request rejected: invalid HTTP Host header:"));
            REQUIRE(entries.front().message().find("node config") == std::string::npos);
        }
        // Reset a real client only after its request reaches the origin. Compression
        // and passthrough streams must record its departure at info level.
        for (const auto compress_response : {false, true}) {
            asio::io_context reset_context;
            asio::ip::tcp::acceptor reset_acceptor(reset_context,
                {asio::ip::address_v4::loopback(), 0});
            auto reset_config = snapshot(false);
            reset_config.setGeneration(next_generation++);
            auto* website = reset_config.mutableWebsite();
            website->set_response_compression_enabled(compress_response);
            website->set_health_check_enabled(false);
            website->mutable_origins(0)->set_host("127.0.0.1");
            website->mutable_origins(0)->set_port(reset_acceptor.local_endpoint().port());
            publish(std::move(reset_config));
            std::atomic<bool> origin_received{};
            std::atomic<bool> client_reset{};
            std::jthread reset_origin([&](std::stop_token stop) {
                asio::ip::tcp::socket socket(reset_context);
                if (!accept_until_stopped(reset_acceptor, socket, stop)) {
                    return;
                }
                std::array<char, 4096> bytes{};
                std::error_code error;
                if (socket.read_some(asio::buffer(bytes), error) == 0 || error) {
                    return;
                }
                origin_received = true;
                while (!client_reset && !stop.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                const std::string reply =
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
                asio::write(socket, asio::buffer(reply), error);
            });
            asio::ip::tcp::socket client(reset_context);
            client.connect({asio::ip::address_v4::loopback(), port});
            const std::string input =
                "GET /client-reset HTTP/1.1\r\nHost: www.example.com\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";
            asio::write(client, asio::buffer(input));
            REQUIRE(waitUntil([&] { return origin_received.load(); }, std::chrono::seconds(2)));
            client.set_option(asio::socket_base::linger(true, 0));
            client.close();
            client_reset = true;
            reset_origin.join();
            REQUIRE(connections_reach(0));
            const auto entries = collect_node_logs();
            REQUIRE(entries.size() == 1);
            REQUIRE(entries.front().level() == "info");
            REQUIRE(entries.front().message().starts_with("client relay write failed:"));
        }
        // An origin resetting the connection remains an actual upstream failure.
        {
            asio::io_context reset_context;
            asio::ip::tcp::acceptor reset_acceptor(reset_context,
                {asio::ip::address_v4::loopback(), 0});
            auto reset_config = snapshot(false);
            reset_config.setGeneration(next_generation++);
            auto* website = reset_config.mutableWebsite();
            website->set_response_compression_enabled(false);
            website->set_health_check_enabled(false);
            website->mutable_origins(0)->set_host("127.0.0.1");
            website->mutable_origins(0)->set_port(reset_acceptor.local_endpoint().port());
            publish(std::move(reset_config));
            std::jthread reset_origin([&](std::stop_token stop) {
                asio::ip::tcp::socket socket(reset_context);
                if (!accept_until_stopped(reset_acceptor, socket, stop)) {
                    return;
                }
                std::array<char, 4096> bytes{};
                std::error_code error;
                (void)socket.read_some(asio::buffer(bytes), error);
                socket.set_option(asio::socket_base::linger(true, 0), error);
                socket.close(error);
            });
            REQUIRE(request(port).starts_with("HTTP/1.1 502 Bad Gateway\r\n"));
            reset_origin.join();
            REQUIRE(connections_reach(0));
            const auto entries = collect_node_logs();
            REQUIRE(entries.size() == 1);
            REQUIRE(entries.front().level() == "error");
            REQUIRE(entries.front().message().starts_with("origin response head read failed:"));
        }
        const auto observed_load = load_of(connected_workers);
        REQUIRE(observed_load && observed_load->connection_count_ == 0 &&
                observed_load->traffic_out_bytes_ > 0);
        REQUIRE(observed_load->logs_.published_events_ > 0);
        REQUIRE(observed_load->logs_.queued_events_ == 0);
        REQUIRE(observed_load->logs_.dropped_events_ == 0);
        const auto observed_metrics = metrics_sampler.sample(observed_load->traffic_out_bytes_);
        REQUIRE(observed_metrics.traffic_out_bps_ > 0);
        REQUIRE(observed_metrics.cpu_usage_ >= 0 && observed_metrics.cpu_usage_ <= 1);
        REQUIRE(observed_metrics.memory_usage_ >= 0 && observed_metrics.memory_usage_ <= 1);
        REQUIRE(observed_metrics.cpu_count_ && *observed_metrics.cpu_count_ > 0);
        REQUIRE(observed_metrics.memory_total_bytes_ && *observed_metrics.memory_total_bytes_ > 0);
        REQUIRE(observed_metrics.disk_total_bytes_ && *observed_metrics.disk_total_bytes_ > 0);
        REQUIRE(observed_metrics.disk_usage_ && *observed_metrics.disk_usage_ >= 0 && *observed_metrics.disk_usage_ <= 1);
        // Without a collected load no traffic rate is reported.
        REQUIRE(metrics_sampler.sample(std::nullopt).traffic_out_bps_ == 0);

        // Per-IP accounting over real sockets, collected through connected_workers.
        using flexedge::node::connected_clients;
        REQUIRE(connected_clients::get_key(asio::ip::make_address("127.0.0.1")) ==
                connected_clients::get_key(asio::ip::make_address("::ffff:127.0.0.1")));
        REQUIRE(connected_clients::get_key(asio::ip::make_address("::1")) !=
                connected_clients::get_key(asio::ip::make_address("127.0.0.1")));
        {
            connected_clients registry;
            const auto client = connected_clients::get_key(asio::ip::make_address("192.0.2.1"));
            registry.opened(client);
            registry.opened(client);
            registry.closed(client);
            REQUIRE(registry.snapshot().size() == 1);
            registry.closed(client);
            REQUIRE(registry.snapshot().empty());
            registry.closed(client);
            REQUIRE(registry.snapshot().empty());
        }
        REQUIRE(count_ips({}) == 0);
        REQUIRE(count_ips(connected_workers) == 0);
        asio::io_context client_context;
        {
            // One client connected to both workers is one IP: per worker 1 and 2, together 2.
            auto first_only = flexedge::node::edge_listener::create(loops.loop(0), first_context,
                asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
            auto second_only = flexedge::node::edge_listener::create(loops.loop(1), second_context,
                asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
            start(first_only);
            start(second_only);
            const auto first_port = first_only->local_endpoint().port();
            const auto second_port = second_only->local_endpoint().port();
            auto on_first = connect_from(client_context, "127.0.0.1", "127.0.0.1", first_port);
            auto on_second = connect_from(client_context, "127.0.0.1", "127.0.0.1", second_port);
            auto other_on_second = connect_from(client_context, "127.0.0.2", "127.0.0.1", second_port);
            REQUIRE(connections_reach(3));
            REQUIRE(count_ips(std::span(connected_workers).first(1)) == 1);
            REQUIRE(count_ips(std::span(connected_workers).last(1)) == 2);
            REQUIRE(count_ips(connected_workers) == 2);
            on_first.close();
            REQUIRE(connections_reach(2));
            REQUIRE(count_ips(std::span(connected_workers).first(1)) == 0);
            REQUIRE(count_ips(connected_workers) == 2);
            on_second.close();
            REQUIRE(connected_ips_reach(1));
            other_on_second.close();
            REQUIRE(connected_ips_reach(0));
            first_only->request_stop();
            second_only->request_stop();
        }
        {
            // Connections from one client on the shared port count once wherever SO_REUSEPORT
            // places them, and each stays counted for its socket's lifetime, not per request.
            std::vector<asio::ip::tcp::socket> held;
            for (int index = 0; index < 3; ++index) {
                held.push_back(connect_from(client_context, "127.0.0.1", "127.0.0.1", port));
            }
            held.push_back(connect_from(client_context, "127.0.0.2", "127.0.0.1", port));
            REQUIRE(connections_reach(4));
            REQUIRE(count_ips(connected_workers) == 2);
            held[0].close();
            held[1].close();
            REQUIRE(connections_reach(2));
            REQUIRE(count_ips(connected_workers) == 2);
            held[2].close();
            REQUIRE(connected_ips_reach(1));
            // Asking for keep-alive keeps the client counted until the edge closes its socket
            // after the response.
            auto keep_alive = connect_from(client_context, "127.0.0.3", "127.0.0.1", port);
            REQUIRE(connected_ips_reach(2));
            asio::write(keep_alive, asio::buffer(std::string_view(
                                        "GET / HTTP/1.1\r\nHost: unknown.example.com\r\n"
                                        "Connection: keep-alive\r\n\r\n")));
            REQUIRE(read_until_closed(keep_alive).starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
            REQUIRE(connected_ips_reach(1));
            held[3].close();
            REQUIRE(connected_ips_reach(0));
            REQUIRE(connections_reach(0));
        }
        {
            // IPv6 clients are distinct addresses. An IPv4 client reaching an IPv6 socket appears
            // as ::ffff:127.0.0.1 and is the same client as on an IPv4 listener.
            auto ipv6_listener = flexedge::node::edge_listener::create(loops.loop(1), second_context,
                asio::ip::tcp::endpoint(asio::ip::address_v6::loopback(), 0), nullptr);
            start(ipv6_listener);
            auto ipv4 = connect_from(client_context, "127.0.0.1", "127.0.0.1", port);
            auto ipv6 = connect_from(client_context, "::1", "::1", ipv6_listener->local_endpoint().port());
            REQUIRE(connections_reach(2));
            REQUIRE(count_ips(connected_workers) == 2);
#if !defined(_WIN32)
            // Windows IPv6 sockets are IPv6-only by default and cannot accept IPv4 clients.
            auto mapped_listener = flexedge::node::edge_listener::create(loops.loop(0), first_context,
                asio::ip::tcp::endpoint(asio::ip::make_address("::ffff:127.0.0.1"), 0), nullptr);
            start(mapped_listener);
            auto mapped = connect_from(client_context, "127.0.0.1", "127.0.0.1",
                mapped_listener->local_endpoint().port());
            REQUIRE(connections_reach(3));
            REQUIRE(count_ips(connected_workers) == 2);
            ipv4.close();
            REQUIRE(connections_reach(2));
            REQUIRE(count_ips(connected_workers) == 2);
            mapped.close();
            mapped_listener->request_stop();
#else
            ipv4.close();
#endif
            REQUIRE(connected_ips_reach(1));
            ipv6.close();
            REQUIRE(connected_ips_reach(0));
            ipv6_listener->request_stop();
        }

        auto redirect_snapshot = snapshot(true);
        redirect_snapshot.setGeneration(next_generation++);
        publish(std::move(redirect_snapshot));
        const auto redirect_response = request(port);
        REQUIRE(redirect_response.starts_with("HTTP/1.1 301 Moved Permanently\r\n"));
        REQUIRE(redirect_response.find("Location: https://www.example.com/a?q=1\r\n") !=
                std::string::npos);

        auto route_snapshot = snapshot(false);
        route_snapshot.setGeneration(next_generation++);
        auto* route = route_snapshot.mutableWebsite()->add_route_rules();
        route->set_id("route-1");
        route->set_enabled(true);
        route->set_match_type("prefix");
        route->set_path("/a");
        route->add_methods("GET");
        route->set_action("redirect");
        route->set_redirect_url("/moved");
        route->set_redirect_status(302);
        auto* route_header = route->add_response_headers();
        route_header->set_name("X-Edge-Route");
        route_header->set_value("matched");
        publish(std::move(route_snapshot));
        const auto route_response = request(port);
        REQUIRE(route_response.starts_with("HTTP/1.1 302 Found\r\n"));
        REQUIRE(route_response.find("Location: /moved?q=1\r\n") != std::string::npos);
        REQUIRE(route_response.find("X-Edge-Route: matched\r\n") != std::string::npos);
        for (const auto status : {307u, 308u}) {
            auto advanced = snapshot(false);
            advanced.setGeneration(next_generation++);
            auto* matched = advanced.mutableWebsite()->add_route_rules();
            matched->set_id("regex-post");
            matched->set_enabled(true);
            matched->set_match_type("regex");
            matched->set_path("^/old/(.*)$");
            matched->add_methods("POST");
            matched->set_action("redirect");
            matched->set_redirect_status(status);
            matched->set_redirect_url("/new/${1}#end");
            auto* header = matched->add_conditions();
            header->set_source("header");
            header->set_name("X-Channel");
            header->set_op("equals");
            header->set_value("beta");
            auto* query = matched->add_conditions();
            query->set_source("query");
            query->set_name("q");
            query->set_op("equals");
            query->set_value("a b");
            auto* fallback = advanced.mutableWebsite()->add_route_rules();
            // A later prefix must not outrank the earlier matching regex.
            fallback->set_id("fallback-prefix");
            fallback->set_enabled(true);
            fallback->set_match_type("prefix");
            fallback->set_path("/old");
            fallback->set_action("redirect");
            fallback->set_redirect_status(302);
            fallback->set_redirect_url("/fallback");
            publish(std::move(advanced));
            const auto answer = request(port, "www.example.com", "x-channel: beta\r\n", "/old/a%2Fb?q=a+b", "POST", "payload");
            REQUIRE(answer.starts_with("HTTP/1.1 " + std::to_string(status) + ' ' +
                                       std::string(ruvia::http_reason_phrase(ruvia::http_status_code::from_value(
                                           static_cast<std::uint16_t>(status)))) +
                                       "\r\n"));
            REQUIRE(answer.find("Location: /new/a%2Fb?q=a+b#end\r\n") != std::string::npos);
            REQUIRE(request(port, "www.example.com", "X-Channel: stable\r\n", "/old/a?q=a+b", "POST", "payload").starts_with("HTTP/1.1 302 Found"));
            REQUIRE(request(port, "www.example.com", "X-Channel: beta\r\n", "/old/a?q=other", "POST", "payload").starts_with("HTTP/1.1 302 Found"));
        }

        // Ruvia parses only the request head; the node frames Content-Length and chunked
        // bodies, applies its 2 MiB request budget, and serves one request per connection.
        {
            asio::io_context echo_context;
            asio::ip::tcp::acceptor echo_acceptor(
                echo_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
            std::mutex echo_mutex;
            std::vector<std::string> echoed_bodies;
            std::jthread echo_origin([&](std::stop_token stop_token) {
                for (;;) {
                    asio::ip::tcp::socket socket(echo_context);
                    if (!accept_until_stopped(echo_acceptor, socket, stop_token)) {
                        return;
                    }
                    std::string input;
                    std::array<char, 16384> buffer{};
                    std::error_code error;
                    while (!error && input.find("\r\n\r\n") == std::string::npos) {
                        const auto size = socket.read_some(asio::buffer(buffer), error);
                        input.append(buffer.data(), size);
                    }
                    if (error) {
                        continue;
                    }
                    const auto body_offset = input.find("\r\n\r\n") + 4;
                    auto head = input.substr(0, body_offset);
                    for (auto& ch : head) {
                        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    }
                    std::size_t length{};
                    if (const auto field = head.find("\r\ncontent-length:"); field != std::string::npos) {
                        length = std::stoul(head.substr(field + 17));
                    }
                    while (!error && input.size() - body_offset < length) {
                        const auto size = socket.read_some(asio::buffer(buffer), error);
                        input.append(buffer.data(), size);
                    }
                    if (error) {
                        continue;
                    }
                    const auto body = input.substr(body_offset, length);
                    {
                        std::lock_guard lock(echo_mutex);
                        echoed_bodies.push_back(body);
                    }
                    const auto response = "HTTP/1.1 200 OK\r\nContent-Length: " +
                                          std::to_string(body.size()) +
                                          "\r\nConnection: close\r\n\r\n" + body;
                    asio::write(socket, asio::buffer(response), error);
                }
            });
            auto framing_snapshot = snapshot(false);
            framing_snapshot.setGeneration(next_generation++);
            framing_snapshot.mutableWebsite()->mutable_origins(0)->set_host("127.0.0.1");
            framing_snapshot.mutableWebsite()->mutable_origins(0)->set_port(
                echo_acceptor.local_endpoint().port());
            publish(std::move(framing_snapshot));
            const auto echoed_count = [&] {
                std::lock_guard lock(echo_mutex);
                return echoed_bodies.size();
            };
            const std::string head = "POST /echo HTTP/1.1\r\nHost: www.example.com\r\n";

            const auto known = exchange_raw(port, {head + "Content-Length: 5\r\n\r\nhe", "llo"});
            REQUIRE(known.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(response_body(known) == "hello");
            REQUIRE(echoed_count() == 1);

            const auto chunked = exchange_raw(
                port, {head + "Transfer-Encoding: chunked\r\n\r\n4\r\nWi",
                          "ki\r\n5;name=value\r\npedia\r\n0\r\nX-Trailer: value\r\n", "\r\n"});
            REQUIRE(chunked.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(response_body(chunked) == "Wikipedia");
            REQUIRE(echoed_count() == 2);

            for (const std::string_view invalid :
                {"zz\r\nWiki\r\n0\r\n\r\n", "4\r\nWikiXX0\r\n\r\n", "4\nWiki\r\n0\r\n\r\n",
                    "4\r\nWiki\r\n0\r\nbad trailer\r\n\r\n"}) {
                const auto invalid_chunk_response = exchange_raw(
                    port, {head + "Transfer-Encoding: chunked\r\n\r\n" + std::string(invalid)});
                REQUIRE(invalid_chunk_response.starts_with("HTTP/1.1 400 Bad Request\r\n"));
            }
            REQUIRE(echoed_count() == 2);

            constexpr std::size_t max_request_bytes = 2 * 1024 * 1024;
            const auto length_head = [&](std::size_t length) {
                return head + "Content-Length: " + std::to_string(length) + "\r\n\r\n";
            };
            // Seven length digits keep the head size fixed for the boundary pair.
            const auto boundary_length = max_request_bytes - length_head(1000000).size();
            REQUIRE(length_head(boundary_length).size() + boundary_length == max_request_bytes);
            const auto at_limit = exchange_raw(
                port, {length_head(boundary_length) + std::string(boundary_length, 'b')});
            REQUIRE(at_limit.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(response_body(at_limit) == std::string(boundary_length, 'b'));
            REQUIRE(echoed_count() == 3);
            const auto over_limit = exchange_raw(port, {length_head(boundary_length + 1)});
            REQUIRE(over_limit.starts_with("HTTP/1.1 413 Content Too Large\r\n"));
            const auto over_framing_ceiling = exchange_raw(port, {length_head(16 * 1024 * 1024 + 1)});
            REQUIRE(over_framing_ceiling.starts_with("HTTP/1.1 400 Bad Request\r\n"));
            const auto oversized_chunk =
                exchange_raw(port, {head + "Transfer-Encoding: chunked\r\n\r\n1000001\r\n"});
            REQUIRE(oversized_chunk.starts_with("HTTP/1.1 400 Bad Request\r\n"));
            const auto unfinished_chunk_head =
                head + "Transfer-Encoding: chunked\r\n\r\n200000\r\n";
            const auto unfinished_chunk = exchange_raw(
                port, {unfinished_chunk_head +
                          std::string(max_request_bytes - unfinished_chunk_head.size(), 'c')});
            REQUIRE(unfinished_chunk.starts_with("HTTP/1.1 413 Content Too Large\r\n"));
            REQUIRE(echoed_count() == 3);

            // The response closes the connection; a pipelined request is never served and
            // its bytes are not forwarded with the first body.
            const auto pipelined = exchange_raw(
                port, {head + "Content-Length: 5\r\n\r\nhello"
                              "GET /second HTTP/1.1\r\nHost: www.example.com\r\n\r\n"});
            REQUIRE(pipelined.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(pipelined.find("Connection: close\r\n") != std::string::npos);
            REQUIRE(pipelined.find("HTTP/1.1 ", 1) == std::string::npos);
            REQUIRE(response_body(pipelined) == "hello");
            REQUIRE(echoed_count() == 4);

            const auto waiting_upload = [&](std::string_view framing, std::string_view body) {
                asio::ip::tcp::socket socket(echo_context);
                socket.connect({asio::ip::address_v4::loopback(), port});
                const auto headers = head +
                                     "Expect: , 100-CONTINUE,\r\nExpect: 100-continue\r\n" +
                                     std::string(framing) + "\r\n";
                asio::write(socket, asio::buffer(headers));
                std::string interim;
                std::array<char, 1024> buffer;
                REQUIRE(waitUntil([&] {
                    std::error_code error;
                    if (socket.available(error) != 0) {
                        const auto size = socket.read_some(asio::buffer(buffer), error);
                        interim.append(buffer.data(), size);
                    }
                    if (error) {
                        throw std::system_error(error);
                    }
                    return interim.find("\r\n\r\n") != std::string::npos;
                },
                    std::chrono::seconds(3)));
                REQUIRE(interim.starts_with("HTTP/1.1 100 "));
                asio::write(socket, asio::buffer(body.substr(0, body.size() / 2)));
                asio::write(socket, asio::buffer(body.substr(body.size() / 2)));
                std::string final;
                std::error_code error;
                while (!error) {
                    const auto size = socket.read_some(asio::buffer(buffer), error);
                    final.append(buffer.data(), size);
                }
                REQUIRE(error == asio::error::eof);
                REQUIRE(final.starts_with("HTTP/1.1 200 "));
                REQUIRE(response_body(final) == "hello");
            };
            waiting_upload("Content-Length: 5\r\n", "hello");
            waiting_upload("Transfer-Encoding: chunked\r\n", "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n");
            const auto http10 = exchange_raw(port, {"POST /echo HTTP/1.0\r\nHost: www.example.com\r\n"
                                                    "Expect: 100-continue\r\nContent-Length: 5\r\n\r\nhello"});
            REQUIRE(http10.starts_with("HTTP/1.1 200 "));
            REQUIRE(response_body(http10) == "hello");
            const auto empty_expect = exchange_raw(port, {head + "Expect: 100-continue\r\nContent-Length: 0\r\n\r\n"});
            REQUIRE(empty_expect.starts_with("HTTP/1.1 200 "));
            REQUIRE(response_body(empty_expect).empty());
            echo_origin.request_stop();
        }

        asio::io_context origin_context;
        asio::ip::tcp::acceptor origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto origin_port = origin_acceptor.local_endpoint().port();
        asio::ip::tcp::acceptor unavailable_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto unavailable_port = unavailable_acceptor.local_endpoint().port();
        unavailable_acceptor.close();
        std::atomic<bool> saw_route_rewrite{false};
        std::jthread origin_server([&](std::stop_token stop_token) {
            asio::ip::tcp::socket origin_socket(origin_context);
            if (!accept_until_stopped(origin_acceptor, origin_socket, stop_token)) {
                return;
            }
            std::array<char, 4096> origin_request{};
            std::error_code ignored;
            const auto received = origin_socket.read_some(asio::buffer(origin_request), ignored);
            saw_route_rewrite.store(std::string_view(origin_request.data(), received).starts_with("GET /internal/users?route=1 HTTP/1.1\r\n"));
            constexpr std::string_view origin_response =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: Foo\r\n"
                "Foo: remove-me\r\nX-Origin: retained\r\n\r\nOK";
            asio::write(origin_socket, asio::buffer(origin_response));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        });
        auto proxy_snapshot = snapshot(false);
        proxy_snapshot.setGeneration(next_generation++);
        proxy_snapshot.mutableWebsite()->mutable_origins(0)->set_host("127.0.0.1");
        proxy_snapshot.mutableWebsite()->mutable_origins(0)->set_port(unavailable_port);
        auto* backup = proxy_snapshot.mutableWebsite()->add_origins();
        backup->set_id("origin-backup");
        backup->set_protocol("http");
        backup->set_host("127.0.0.1");
        backup->set_port(origin_port);
        backup->set_role("backup");
        backup->set_weight(100);
        backup->set_enabled(true);
        auto* proxy_route = proxy_snapshot.mutableWebsite()->add_route_rules();
        proxy_route->set_id("route-backup");
        proxy_route->set_enabled(true);
        proxy_route->set_match_type("regex");
        proxy_route->set_path("^/a/(.*)$");
        proxy_route->set_action("proxy");
        proxy_route->add_origin_ids("origin-backup");
        proxy_route->add_hostnames("www.example.com");
        proxy_route->set_rewrite_mode("replace_path");
        proxy_route->set_rewrite_path("/internal/${1}");
        proxy_route->set_query_mode("replace");
        proxy_route->set_query_string("route=1");
        auto* source_route = proxy_snapshot.mutableWebsite()->add_route_rules();
        *source_route = *proxy_route;
        source_route->set_id("select-rewritten-path");
        source_route->set_match_type("prefix");
        source_route->set_path("/internal");
        source_route->set_rewrite_mode("none");
        source_route->clear_rewrite_path();
        source_route->set_query_mode("preserve");
        source_route->clear_query_string();
        proxy_route->set_action("rewrite");
        proxy_route->clear_origin_ids();
        const flexedge::node::route_program proxy_program(proxy_snapshot.website());
        const auto route_policy = flexedge::node::evaluate_route_rules(
            proxy_program, proxy_snapshot.website(), "GET", "/a/users?q=1", "www.example.com");
        REQUIRE(route_policy);
        flexedge::node::origin_cursor candidates(route_policy->origins(), 0, nullptr);
        const auto* selected_origin = candidates.next();
        REQUIRE(selected_origin != nullptr);
        REQUIRE(selected_origin->id() == "origin-backup");
        REQUIRE(candidates.next() == nullptr);
        publish(std::move(proxy_snapshot));
        const auto proxy_started = std::chrono::steady_clock::now();
        const auto proxy_response = request(port, "WWW.Example.COM:80", {}, "/a/users?q=1");
        REQUIRE(saw_route_rewrite.load());
        const auto proxy_elapsed = std::chrono::steady_clock::now() - proxy_started;
        REQUIRE(proxy_response.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(proxy_response.ends_with("\r\n\r\nOK"));
        REQUIRE(proxy_response.find("Connection: close\r\n") != std::string::npos);
        REQUIRE(proxy_response.find("X-Origin: retained\r\n") != std::string::npos);
        REQUIRE(proxy_response.find("Foo: remove-me\r\n") == std::string::npos);
        REQUIRE(proxy_elapsed < std::chrono::seconds(1));

        // A request must retain its route and remaining origin plan across publication,
        // including a failover that happens only after both workers adopt the new config.
        {
            asio::ip::tcp::acceptor held_primary(origin_context,
                asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
            asio::ip::tcp::acceptor retained_backup(origin_context,
                asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
            std::atomic<bool> primary_received{};
            std::atomic<bool> release_primary{};
            std::atomic<bool> backup_received_rewrite{};
            std::jthread held_origin([&](std::stop_token stop_token) {
                asio::ip::tcp::socket primary_socket(origin_context);
                if (!accept_until_stopped(held_primary, primary_socket, stop_token)) {
                    return;
                }
                std::string origin_head;
                std::error_code ignored;
                (void)asio::read_until(primary_socket, asio::dynamic_buffer(origin_head),
                    "\r\n\r\n", ignored);
                primary_received.store(true);
                while (!release_primary.load() && !stop_token.stop_requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                primary_socket.close(ignored);
                if (stop_token.stop_requested()) {
                    return;
                }
                asio::ip::tcp::socket backup_socket(origin_context);
                if (!accept_until_stopped(retained_backup, backup_socket, stop_token)) {
                    return;
                }
                origin_head.clear();
                (void)asio::read_until(backup_socket, asio::dynamic_buffer(origin_head),
                    "\r\n\r\n", ignored);
                backup_received_rewrite.store(origin_head.starts_with(
                    "GET /retained.txt?old=1 HTTP/1.1\r\n"));
                constexpr std::string_view response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 9\r\nConnection: close\r\n\r\nold route";
                asio::write(backup_socket, asio::buffer(response));
            });
            auto retained = snapshot(false);
            retained.setGeneration(next_generation++);
            auto* retained_website = retained.mutableWebsite();
            auto* primary = retained_website->mutable_origins(0);
            primary->set_host("127.0.0.1");
            primary->set_port(held_primary.local_endpoint().port());
            auto* old_backup = retained_website->add_origins();
            *old_backup = *primary;
            old_backup->set_id("retained-backup");
            old_backup->set_role("backup");
            old_backup->set_port(retained_backup.local_endpoint().port());
            auto* old_rule = retained_website->add_route_rules();
            old_rule->set_id("retained-route");
            old_rule->set_enabled(true);
            old_rule->set_action("proxy");
            old_rule->set_origin_group("default");
            old_rule->set_match_type("exact");
            old_rule->set_path("/held");
            old_rule->set_rewrite_mode("replace_path");
            old_rule->set_rewrite_path("/retained.txt");
            old_rule->set_query_mode("replace");
            old_rule->set_query_string("old=1");
            // Proxy matching occurs after the rewrite phase.
            auto* retained_proxy = retained_website->add_route_rules();
            retained_proxy->set_id("retained-response-policy");
            retained_proxy->set_enabled(true);
            retained_proxy->set_action("proxy");
            retained_proxy->set_origin_group("default");
            retained_proxy->set_match_type("exact");
            retained_proxy->set_path("/retained.txt");
            auto* old_header = retained_proxy->add_response_headers();
            old_header->set_name("X-Retained-Policy");
            old_header->set_value("old-request-retained-execution-header");
            publish(std::move(retained));

            asio::ip::tcp::socket held_client(origin_context);
            held_client.connect({asio::ip::address_v4::loopback(), port});
            constexpr std::string_view held_request =
                "GET /held HTTP/1.1\r\nHost: www.example.com\r\nConnection: close\r\n\r\n";
            asio::write(held_client, asio::buffer(held_request));
            REQUIRE(waitUntil([&] { return primary_received.load(); }, std::chrono::seconds(2)));
            auto replacement_config = snapshot(false);
            replacement_config.setGeneration(next_generation++);
            auto* replacement_rule = replacement_config.mutableWebsite()->add_route_rules();
            replacement_rule->set_id("replacement-redirect");
            replacement_rule->set_enabled(true);
            replacement_rule->set_action("redirect");
            replacement_rule->set_match_type("exact");
            replacement_rule->set_path("/held");
            replacement_rule->set_redirect_status(307);
            replacement_rule->set_redirect_url("/new-config");
            publish(std::move(replacement_config));
            release_primary.store(true);

            std::string retained_response;
            std::array<char, 4096> bytes{};
            std::error_code error;
            while (!error) {
                const auto received = held_client.read_some(asio::buffer(bytes), error);
                retained_response.append(bytes.data(), received);
            }
            REQUIRE(error == asio::error::eof);
            REQUIRE(backup_received_rewrite.load());
            REQUIRE(retained_response.starts_with("HTTP/1.1 200 OK\r\n"));
            REQUIRE(retained_response.find(
                        "X-Retained-Policy: old-request-retained-execution-header\r\n") !=
                    std::string::npos);
            REQUIRE(retained_response.ends_with("\r\n\r\nold route"));
            const auto replacement_response = request(port, "www.example.com", {}, "/held");
            REQUIRE(replacement_response.starts_with("HTTP/1.1 307 Temporary Redirect\r\n"));
            REQUIRE(replacement_response.find("Location: /new-config\r\n") != std::string::npos);
        }

        asio::ip::tcp::acceptor compression_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto compression_port = compression_acceptor.local_endpoint().port();
        std::atomic<bool> saw_single_rewrite{false};
        std::atomic<bool> received_compressed_prefix{false};
        std::atomic<bool> streamed_before_origin_finished{false};
        std::jthread compression_server([&](std::stop_token stop_token) {
            asio::ip::tcp::socket socket(origin_context);
            if (!accept_until_stopped(compression_acceptor, socket, stop_token)) {
                return;
            }
            std::array<char, 4096> origin_request{};
            std::error_code ignored;
            const auto received = socket.read_some(asio::buffer(origin_request), ignored);
            saw_single_rewrite.store(std::string_view(origin_request.data(), received).starts_with("GET /a/once?q=1 HTTP/1.1\r\n"));
            const std::string body(4096, 'a');
            const auto response =
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4096\r\n"
                "Connection: close\r\n\r\n" +
                body.substr(0, 2048);
            asio::write(socket, asio::buffer(response));
            streamed_before_origin_finished.store(waitUntil(
                [&] { return received_compressed_prefix.load(); }, std::chrono::seconds(3)));
            asio::write(socket, asio::buffer(body.data() + 2048, body.size() - 2048));
        });
        auto compression_snapshot = snapshot(false);
        compression_snapshot.setGeneration(next_generation++);
        auto* compression_origin = compression_snapshot.mutableWebsite()->mutable_origins(0);
        compression_origin->set_host("127.0.0.1");
        compression_origin->set_port(compression_port);
        auto* compression_route = compression_snapshot.mutableWebsite()->add_route_rules();
        compression_route->set_id("rewrite-once");
        compression_route->set_enabled(true);
        compression_route->set_match_type("prefix");
        compression_route->set_path("/a");
        compression_route->set_action("proxy");
        compression_route->set_origin_group("default");
        compression_route->set_rewrite_mode("replace_prefix");
        compression_route->set_rewrite_path("/a/once");
        publish(std::move(compression_snapshot));
        const auto compressed_proxy_response = [&] {
            asio::ip::tcp::socket socket(origin_context);
            socket.connect({asio::ip::address_v4::loopback(), port});
            const std::string incoming = "GET /a?q=1 HTTP/1.1\r\nHost: www.example.com\r\nAccept-Encoding: gzip\r\nConnection: close\r\n\r\n";
            asio::write(socket, asio::buffer(incoming));
            std::string response;
            std::array<char, 4096> buffer{};
            std::error_code error;
            while (!error) {
                const auto size = socket.read_some(asio::buffer(buffer), error);
                response.append(buffer.data(), size);
                const auto head_end = response.find("\r\n\r\n");
                if (head_end != std::string::npos && response.size() > head_end + 40) {
                    received_compressed_prefix.store(true);
                }
            }
            REQUIRE(error == asio::error::eof);
            return response;
        }();
        REQUIRE(streamed_before_origin_finished.load());
        REQUIRE(compressed_proxy_response.find("Content-Encoding: gzip\r\n") != std::string::npos);
        REQUIRE(saw_single_rewrite.load());
        REQUIRE(compressed_proxy_response.find("Vary: Accept-Encoding\r\n") != std::string::npos);
        const auto compressed_body_offset = compressed_proxy_response.find("\r\n\r\n");
        REQUIRE(compressed_body_offset != std::string::npos);
        auto decoded_proxy_response = ruvia::decode_http_content(
            ruvia::http_content_coding::gzip,
            [&] {
                ruvia::http1_chunked_body_decoder decoder;
                std::string decoded;
                auto input = std::string_view(compressed_proxy_response).substr(compressed_body_offset + 4);
                for (;;) {
                    const auto step = decoder.decode(input);
                    if (const auto* chunk = step.body_chunk()) {
                        decoded.append(chunk->bytes());
                        input.remove_prefix(chunk->consumed_bytes());
                    } else {
                        REQUIRE(step.complete());
                        break;
                    }
                }
                return decoded;
            }(),
            {.max_decoded_bytes_ = 4096});
        REQUIRE(decoded_proxy_response.decoded() != nullptr);
        REQUIRE(decoded_proxy_response.decoded()->bytes() == std::string(4096, 'a'));

        asio::ip::tcp::acceptor health_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto health_port = health_acceptor.local_endpoint().port();
        std::atomic<int> health_requests{};
        std::jthread health_server([&](std::stop_token stop_token) {
            for (int request_index = 0; request_index < 1; ++request_index) {
                asio::ip::tcp::socket health_socket(origin_context);
                if (!accept_until_stopped(health_acceptor, health_socket, stop_token)) {
                    return;
                }
                std::array<char, 4096> health_request{};
                std::error_code ignored;
                (void)health_socket.read_some(asio::buffer(health_request), ignored);
                ++health_requests;
                constexpr std::string_view health_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                asio::write(health_socket, asio::buffer(health_response));
            }
        });
        auto health_snapshot = snapshot(false);
        health_snapshot.setGeneration(next_generation++);
        auto* health_website = health_snapshot.mutableWebsite();
        health_website->set_health_check_enabled(true);
        health_website->set_healthy_threshold(1);
        health_website->set_unhealthy_threshold(1);
        health_website->mutable_origins(0)->set_host("127.0.0.1");
        health_website->mutable_origins(0)->set_port(health_port);
        publish(std::move(health_snapshot));
        auto node_health =
            flexedge::node::origin_health_supervisor::create(loops.loop(0), first_runtime, first_origin_tls);
        const flexedge::node::origin_health_key_set health_keys{
            {.website_id_ = "website-1", .origin_id_ = "origin-1"}};
        set_health_origins(loops.loop(0), *node_health, health_keys);
        record_health_failure(loops.loop(0), *node_health);
        REQUIRE(!settled_health(loops.loop(0), *node_health)->healthy({"website-1", "origin-1"}));
        node_health->request_start();
        const auto node_healthy = waitUntil(
            [&] { return settled_health(loops.loop(0), *node_health)->healthy({"website-1", "origin-1"}); },
            std::chrono::seconds(3));
        if (!node_healthy) {
            std::cerr << "health requests=" << health_requests.load() << '\n';
        }
        REQUIRE(node_healthy);
        REQUIRE(health_requests.load() == 1);
        node_health->request_stop();

        const auto www_identity = make_identity("www.example.com", 1);
        const auto api_identity = make_identity("api.example.com", 2);
        asio::ip::tcp::acceptor tls_origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto tls_origin_port = tls_origin_acceptor.local_endpoint().port();
        std::jthread tls_origin_server([&](std::stop_token stop_token) {
            for (int request_index = 0; request_index < 12; ++request_index) {
                asio::ip::tcp::socket origin_socket(origin_context);
                if (!accept_until_stopped(tls_origin_acceptor, origin_socket, stop_token)) {
                    return;
                }
                std::array<char, 4096> origin_request{};
                std::error_code ignored;
                (void)origin_socket.read_some(asio::buffer(origin_request), ignored);
                constexpr std::string_view origin_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nTLS";
                asio::write(origin_socket, asio::buffer(origin_response));
            }
        });
        auto tls_snapshot = snapshot(false);
        tls_snapshot.setGeneration(next_generation++);
        auto* tls_website = tls_snapshot.mutableWebsite();
        tls_website->set_hsts_enabled(true);
        tls_website->set_unhealthy_threshold(1);
        tls_website->set_healthy_threshold(1);
        tls_website->mutable_origins(0)->set_host("127.0.0.1");
        tls_website->mutable_origins(0)->set_port(tls_origin_port);
        tls_snapshot.mutableCertificate(0)->set_certificate_chain_pem(www_identity.certificate_);
        tls_snapshot.mutableCertificate(0)->set_private_key_pem(www_identity.private_key_);
        auto* api_certificate = tls_snapshot.addCertificate();
        api_certificate->set_id("certificate-2");
        api_certificate->set_certificate_chain_pem(api_identity.certificate_);
        api_certificate->set_private_key_pem(api_identity.private_key_);
        auto* api_domain = tls_snapshot.mutableWebsite()->add_domains();
        api_domain->set_hostname("api.example.com");
        api_domain->set_https_enabled(true);
        api_domain->set_certificate_digest(flexedge::node::artifact_digest(
            flexedge::node::serialize_artifact(tls_snapshot.contents.back())));
        publish(std::move(tls_snapshot));
        flexedge::node::tls_context_registry tls_contexts;
        publish_tls(loops.loop(0), tls_contexts, runtime.config());
        auto https_listener = flexedge::node::edge_listener::create(loops.loop(0),
            session_context(*node_health, first_metrics, first_clients, first_worker_logs,
                first_origin_connections, request_buffers),
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), &tls_contexts);
        const auto https_port = https_listener->local_endpoint().port();
        start(https_listener);
        // Downstream ALPN selects HTTP/2 when offered and keeps HTTP/1.1 available.
        REQUIRE(tls_alpn(https_port, "www.example.com", true) == "h2");
        REQUIRE(tls_alpn(https_port, "www.example.com", false) == "http/1.1");
        const auto www_response = tls_request(https_port, "www.example.com");
        REQUIRE(www_response.peer_common_name_ == "www.example.com");
        REQUIRE(www_response.bytes_.ends_with("\r\n\r\nTLS"));
        REQUIRE(www_response.bytes_.find("Strict-Transport-Security: max-age=31536000\r\n") !=
                std::string::npos);
        const auto api_response = tls_request(https_port, "api.example.com");
        REQUIRE(api_response.peer_common_name_ == "api.example.com");
        REQUIRE(api_response.bytes_.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(tls_request(https_port, "WWW.Example.COM", "www.example.com.:443").bytes_.starts_with("HTTP/1.1 200 "));
        // A valid SNI must not front another bound domain or an unbound Host.
        for (const auto host : {"unknown.example.com", "api.example.com"}) {
            const auto mismatched_host_response = tls_request(https_port, "www.example.com", host).bytes_;
            REQUIRE(mismatched_host_response.starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
            REQUIRE(mismatched_host_response.ends_with("Content-Length: 0\r\nConnection: close\r\n\r\n"));
        }
        for (const auto version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            require_tls_rejection(https_port, "unknown.example.com", version, SSL_AD_UNRECOGNIZED_NAME);
            require_tls_rejection(https_port, "www.example.com:443", version, SSL_AD_UNRECOGNIZED_NAME);
            require_tls_rejection(https_port, {}, version,
                version == TLS1_3_VERSION ? SSL_AD_MISSING_EXTENSION : SSL_AD_HANDSHAKE_FAILURE);
        }
        const auto session = reusable_tls_session(https_port);
        (void)reusable_tls_session(https_port, session.get());
        require_tls_rejection(https_port, "unknown.example.com", TLS1_2_VERSION, SSL_AD_UNRECOGNIZED_NAME, session.get());
        require_tls_rejection(https_port, {}, TLS1_2_VERSION, SSL_AD_HANDSHAKE_FAILURE, session.get());

        bool unknown_sni_rejected = false;
        try {
            (void)tls_request(https_port, "unknown.example.com");
        } catch (const std::system_error&) {
            unknown_sni_rejected = true;
        }
        REQUIRE(unknown_sni_rejected);
        {
            // A TLS client counts once its handshake completes, like the connection count.
            REQUIRE(connected_ips_reach(0));
            asio::ssl::context tls_client(asio::ssl::context::tls_client);
            tls_client.set_verify_mode(asio::ssl::verify_none);
            asio::ssl::stream<asio::ip::tcp::socket> held(client_context, tls_client);
            REQUIRE(SSL_set_tlsext_host_name(held.native_handle(), "www.example.com") == 1);
            held.lowest_layer().open(asio::ip::tcp::v4());
            held.lowest_layer().bind({asio::ip::make_address("127.0.0.5"), 0});
            held.lowest_layer().connect({asio::ip::address_v4::loopback(), https_port});
            REQUIRE(count_ips(connected_workers) == 0);
            held.handshake(asio::ssl::stream_base::client);
            REQUIRE(connected_ips_reach(1));
            REQUIRE(count_ips(std::span(connected_workers).first(1)) == 1);
            held.lowest_layer().close();
            REQUIRE(connected_ips_reach(0));
        }

        const auto wildcard_identity = make_identity("*.example.com", 5);
        auto wildcard_tls_snapshot = snapshot(false);
        wildcard_tls_snapshot.setGeneration(next_generation++);
        auto* wildcard_tls_website = wildcard_tls_snapshot.mutableWebsite();
        wildcard_tls_website->mutable_origins(0)->set_host("127.0.0.1");
        wildcard_tls_website->mutable_origins(0)->set_port(tls_origin_port);
        wildcard_tls_website->mutable_domains(0)->set_hostname("*.Example.COM.");
        wildcard_tls_snapshot.mutableCertificate(0)->set_certificate_chain_pem(wildcard_identity.certificate_);
        wildcard_tls_snapshot.mutableCertificate(0)->set_private_key_pem(wildcard_identity.private_key_);
        auto* exact_certificate = wildcard_tls_snapshot.addCertificate();
        exact_certificate->set_id("exact-certificate");
        exact_certificate->set_certificate_chain_pem(api_identity.certificate_);
        exact_certificate->set_private_key_pem(api_identity.private_key_);
        auto* exact_tls_domain = wildcard_tls_snapshot.mutableWebsite()->add_domains();
        exact_tls_domain->set_hostname("api.example.com");
        exact_tls_domain->set_https_enabled(true);
        exact_tls_domain->set_certificate_digest(flexedge::node::artifact_digest(
            flexedge::node::serialize_artifact(wildcard_tls_snapshot.contents.back())));
        publish(std::move(wildcard_tls_snapshot));
        publish_tls(loops.loop(0), tls_contexts, runtime.config());
        const auto wildcard_response = tls_request(https_port, "assets.example.com");
        REQUIRE(wildcard_response.peer_common_name_ == "*.example.com");
        REQUIRE(wildcard_response.bytes_.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(tls_request(https_port, "ASSETS.Example.COM", "assets.example.com.:443")
                .bytes_.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(tls_request(https_port, "api.example.com").peer_common_name_ == "api.example.com");
        // Sharing a wildcard certificate does not authorize domain fronting.
        REQUIRE(tls_request(https_port, "assets.example.com", "other.example.com")
                .bytes_.starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        for (const auto version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            for (const auto host : {"example.com", "a.b.example.com", "http.example.com",
                     "*.example.com", "assets.example.com:443"}) {
                require_tls_rejection(https_port, host, version, SSL_AD_UNRECOGNIZED_NAME);
            }
            require_tls_rejection(https_port, {}, version,
                version == TLS1_3_VERSION ? SSL_AD_MISSING_EXTENSION : SSL_AD_HANDSHAKE_FAILURE);
        }

        const auto refreshed_identity = make_identity("refreshed.example.com", 3);
        auto refreshed_snapshot = snapshot(false);
        refreshed_snapshot.setGeneration(next_generation++);
        auto* refreshed_website = refreshed_snapshot.mutableWebsite();
        refreshed_website->set_hsts_enabled(true);
        refreshed_website->mutable_origins(0)->set_host("127.0.0.1");
        refreshed_website->mutable_origins(0)->set_port(tls_origin_port);
        refreshed_snapshot.mutableCertificate(0)->set_certificate_chain_pem(
            refreshed_identity.certificate_);
        refreshed_snapshot.mutableCertificate(0)->set_private_key_pem(refreshed_identity.private_key_);
        publish(std::move(refreshed_snapshot));
        publish_tls(loops.loop(0), tls_contexts, runtime.config());
        // A single configured TLS domain must still require SNI and never expose its certificate.
        for (const auto version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            require_tls_rejection(https_port, {}, version,
                version == TLS1_3_VERSION ? SSL_AD_MISSING_EXTENSION : SSL_AD_HANDSHAKE_FAILURE);
        }
        const auto refreshed_response = tls_request(https_port, "www.example.com");
        REQUIRE(refreshed_response.peer_common_name_ == "refreshed.example.com");
        REQUIRE(refreshed_response.bytes_.starts_with("HTTP/1.1 200 OK\r\n"));
        https_listener->request_stop();

        const auto localhost_identity = make_identity("localhost", 4);
        asio::ssl::context tls_origin_server_context(asio::ssl::context::tls_server);
        tls_origin_server_context.use_certificate_chain(asio::buffer(localhost_identity.certificate_));
        tls_origin_server_context.use_private_key(asio::buffer(localhost_identity.private_key_),
            asio::ssl::context::pem);
        asio::ip::tcp::acceptor secure_origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto secure_origin_port = secure_origin_acceptor.local_endpoint().port();
        std::jthread secure_origin_server([&](std::stop_token stop_token) {
            for (int request_index = 0; request_index < 2; ++request_index) {
                asio::ip::tcp::socket socket(origin_context);
                if (!accept_until_stopped(secure_origin_acceptor, socket, stop_token)) {
                    return;
                }
                asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                    tls_origin_server_context);
                std::error_code error;
                stream.handshake(asio::ssl::stream_base::server, error);
                if (error) {
                    continue;
                }
                std::array<char, 4096> origin_request{};
                (void)stream.read_some(asio::buffer(origin_request), error);
                if (error) {
                    continue;
                }
                constexpr std::string_view origin_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nSECURE";
                asio::write(stream, asio::buffer(origin_response), error);
            }
        });
        auto secure_origin_snapshot = snapshot(false);
        secure_origin_snapshot.setGeneration(next_generation++);
        auto* secure_origin_website = secure_origin_snapshot.mutableWebsite();
        secure_origin_website->set_health_check_enabled(true);
        secure_origin_website->set_healthy_threshold(1);
        secure_origin_website->set_unhealthy_threshold(1);
        secure_origin_website->mutable_origins(0)->set_protocol("https");
        secure_origin_website->mutable_origins(0)->set_host("localhost");
        secure_origin_website->mutable_origins(0)->set_port(secure_origin_port);
        publish(std::move(secure_origin_snapshot));
        flexedge::node::origin_tls_context trusted_origin_tls(localhost_identity.certificate_);
        flexedge::node::origin_connection_pool trusted_origin_connections(loops.loop(0).executor(),
            trusted_origin_tls);
        auto secure_origin_health =
            flexedge::node::origin_health_supervisor::create(loops.loop(0), first_runtime, trusted_origin_tls);
        set_health_origins(loops.loop(0), *secure_origin_health, health_keys);
        const auto secure_origin_context = session_context(*secure_origin_health, first_metrics,
            first_clients, first_worker_logs, trusted_origin_connections, request_buffers);
        auto secure_origin_listener = flexedge::node::edge_listener::create(loops.loop(0),
            secure_origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            nullptr);
        const auto secure_edge_port = secure_origin_listener->local_endpoint().port();
        start(secure_origin_listener);
        const auto secure_origin_response = request(secure_edge_port);
        REQUIRE(secure_origin_response.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(secure_origin_response.ends_with("\r\n\r\nSECURE"));

        record_health_failure(loops.loop(0), *secure_origin_health);
        REQUIRE(!settled_health(loops.loop(0), *secure_origin_health)->healthy({"website-1", "origin-1"}));
        secure_origin_health->request_start();
        REQUIRE(waitUntil(
            [&] { return settled_health(loops.loop(0), *secure_origin_health)->healthy({"website-1", "origin-1"}); },
            std::chrono::seconds(3)));
        secure_origin_health->request_stop();
        secure_origin_listener->request_stop();

        asio::ssl::context buffered_origin_server_context(asio::ssl::context::tls_server);
        buffered_origin_server_context.use_certificate_chain(
            asio::buffer(localhost_identity.certificate_));
        buffered_origin_server_context.use_private_key(asio::buffer(localhost_identity.private_key_),
            asio::ssl::context::pem);
        // Auto mode offers HTTP/2 and HTTP/1.1; this origin declines ALPN and uses HTTP/1.1.
        std::atomic<bool> buffered_origin_offered_both{};
        SSL_CTX_set_alpn_select_cb(
            buffered_origin_server_context.native_handle(),
            [](SSL*, const unsigned char**, unsigned char*, const unsigned char* input,
                unsigned int input_size, void* argument) -> int {
                static_cast<std::atomic<bool>*>(argument)->store(
                    std::string_view(reinterpret_cast<const char*>(input), input_size) ==
                    std::string_view("\x02h2\x08http/1.1", 12));
                return SSL_TLSEXT_ERR_NOACK;
            },
            &buffered_origin_offered_both);
        asio::ip::tcp::acceptor buffered_origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto buffered_origin_port = buffered_origin_acceptor.local_endpoint().port();
        std::atomic<int> buffered_origin_connections{};
        std::atomic<int> buffered_origin_requests{};
        std::jthread buffered_origin_server([&](std::stop_token stop_token) {
            asio::ip::tcp::socket socket(origin_context);
            if (!accept_until_stopped(buffered_origin_acceptor, socket, stop_token)) {
                return;
            }
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                buffered_origin_server_context);
            stream.handshake(asio::ssl::stream_base::server);
            ++buffered_origin_connections;
            for (int request_index = 0; request_index < 2; ++request_index) {
                std::string input;
                std::array<char, 1024> buffer{};
                while (input.find("\r\n\r\n") == std::string::npos) {
                    const auto size = stream.read_some(asio::buffer(buffer));
                    input.append(buffer.data(), size);
                }
                REQUIRE(input.find("Connection: close\r\n") == std::string::npos);
                ++buffered_origin_requests;
                constexpr std::string_view origin_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nH1ORIGIN";
                asio::write(stream, asio::buffer(origin_response));
            }
        });
        auto buffered_origin_snapshot = snapshot(false);
        buffered_origin_snapshot.setGeneration(next_generation++);
        auto* buffered_origin_website = buffered_origin_snapshot.mutableWebsite();
        buffered_origin_website->set_response_compression_enabled(true);
        buffered_origin_website->set_health_check_enabled(false);
        buffered_origin_website->mutable_origins(0)->set_protocol("https");
        buffered_origin_website->mutable_origins(0)->set_host("localhost");
        buffered_origin_website->mutable_origins(0)->set_port(buffered_origin_port);
        publish(std::move(buffered_origin_snapshot));
        auto buffered_origin_listener = flexedge::node::edge_listener::create(loops.loop(0),
            secure_origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0),
            nullptr);
        const auto buffered_origin_edge_port = buffered_origin_listener->local_endpoint().port();
        start(buffered_origin_listener);
        const auto first_buffered_origin_response =
            request(buffered_origin_edge_port, "WWW.Example.COM:80", "Accept-Encoding: gzip\r\n");
        const auto second_buffered_origin_response =
            request(buffered_origin_edge_port, "WWW.Example.COM:80", "Accept-Encoding: gzip\r\n");
        REQUIRE(buffered_origin_offered_both.load());
        REQUIRE(first_buffered_origin_response.ends_with("\r\n\r\nH1ORIGIN"));
        REQUIRE(second_buffered_origin_response.ends_with("\r\n\r\nH1ORIGIN"));
        REQUIRE(buffered_origin_connections.load() == 1);
        REQUIRE(buffered_origin_requests.load() == 2);
        buffered_origin_listener->request_stop();

        asio::ip::tcp::acceptor untrusted_origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto untrusted_origin_port = untrusted_origin_acceptor.local_endpoint().port();
        std::jthread untrusted_origin_server([&](std::stop_token stop_token) {
            asio::ip::tcp::socket socket(origin_context);
            if (!accept_until_stopped(untrusted_origin_acceptor, socket, stop_token)) {
                return;
            }
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                tls_origin_server_context);
            std::error_code ignored;
            stream.handshake(asio::ssl::stream_base::server, ignored);
        });
        auto untrusted_origin_snapshot = snapshot(false);
        untrusted_origin_snapshot.setGeneration(next_generation++);
        auto* untrusted_origin = untrusted_origin_snapshot.mutableWebsite()->mutable_origins(0);
        untrusted_origin->set_protocol("https");
        untrusted_origin->set_host("localhost");
        untrusted_origin->set_port(untrusted_origin_port);
        publish(std::move(untrusted_origin_snapshot));
        const auto untrusted_origin_response = request(port);
        REQUIRE(untrusted_origin_response.starts_with("HTTP/1.1 502 Bad Gateway\r\n"));

        asio::ip::tcp::acceptor web_socket_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto web_socket_port = web_socket_acceptor.local_endpoint().port();
        std::atomic<bool> upgrade_headers_preserved{};
        std::jthread web_socket_origin([&](std::stop_token stop_token) {
            asio::ip::tcp::socket socket(origin_context);
            if (!accept_until_stopped(web_socket_acceptor, socket, stop_token)) {
                return;
            }
            std::string handshake;
            std::array<char, 1024> buffer{};
            while (handshake.find("\r\n\r\n") == std::string::npos) {
                const auto size = socket.read_some(asio::buffer(buffer));
                handshake.append(buffer.data(), size);
            }
            upgrade_headers_preserved =
                handshake.find("Upgrade: websocket\r\n") != std::string::npos &&
                handshake.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos;
            constexpr std::string_view switching_protocols =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
            asio::write(socket, asio::buffer(switching_protocols));
            std::array<char, 4> client_bytes{};
            asio::read(socket, asio::buffer(client_bytes));
            if (std::string_view(client_bytes.data(), client_bytes.size()) == "PING") {
                asio::write(socket, asio::buffer(std::string_view("PONG")));
            }
        });
        auto web_socket_snapshot = snapshot(false);
        web_socket_snapshot.setGeneration(next_generation++);
        auto* web_socket_origin_config = web_socket_snapshot.mutableWebsite()->mutable_origins(0);
        web_socket_origin_config->set_host("127.0.0.1");
        web_socket_origin_config->set_port(web_socket_port);
        publish(std::move(web_socket_snapshot));
        REQUIRE(web_socket_round_trip(port) == "PONG");
        REQUIRE(upgrade_headers_preserved.load());

        asio::ip::tcp::acceptor reusable_origin_acceptor(
            origin_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto reusable_origin_port = reusable_origin_acceptor.local_endpoint().port();
        std::atomic<int> reusable_origin_connections{};
        std::atomic<int> reusable_origin_requests{};
        std::jthread reusable_origin_server([&](std::stop_token stop_token) {
            asio::ip::tcp::socket socket(origin_context);
            if (!accept_until_stopped(reusable_origin_acceptor, socket, stop_token)) {
                return;
            }
            ++reusable_origin_connections;
            for (int request_index = 0; request_index < 2; ++request_index) {
                std::string input;
                std::array<char, 1024> buffer{};
                while (input.find("\r\n\r\n") == std::string::npos) {
                    const auto size = socket.read_some(asio::buffer(buffer));
                    input.append(buffer.data(), size);
                }
                REQUIRE(input.find("Connection: close\r\n") == std::string::npos);
                ++reusable_origin_requests;
                constexpr std::string_view origin_response =
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nREUSE";
                asio::write(socket, asio::buffer(origin_response));
            }
        });
        auto reusable_origin_snapshot = snapshot(false);
        reusable_origin_snapshot.setGeneration(next_generation++);
        auto* reusable_website = reusable_origin_snapshot.mutableWebsite();
        reusable_website->set_response_compression_enabled(false);
        reusable_website->set_health_check_enabled(false);
        reusable_website->mutable_origins(0)->set_host("127.0.0.1");
        reusable_website->mutable_origins(0)->set_port(reusable_origin_port);
        publish(std::move(reusable_origin_snapshot));
        auto reusable_origin_health =
            flexedge::node::origin_health_supervisor::create(loops.loop(0), first_runtime, first_origin_tls);
        auto reusable_edge_listener = flexedge::node::edge_listener::create(loops.loop(0),
            session_context(*reusable_origin_health, first_metrics, first_clients,
                first_worker_logs, first_origin_connections, request_buffers),
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
        const auto reusable_edge_port = reusable_edge_listener->local_endpoint().port();
        start(reusable_edge_listener);
        const auto first_reused_response = request(reusable_edge_port);
        const auto second_reused_response = request(reusable_edge_port);
        REQUIRE(first_reused_response.ends_with("\r\n\r\nREUSE"));
        REQUIRE(second_reused_response.ends_with("\r\n\r\nREUSE"));
        REQUIRE(reusable_origin_connections.load() == 1);
        REQUIRE(reusable_origin_requests.load() == 2);
        reusable_edge_listener->request_stop();
        first_listener->request_stop();
        second_listener->request_stop();
        // Posting cleanup after shutdown must not keep an otherwise unused
        // listener (and its event loop handle) alive indefinitely.
        auto stopped_loop_listener = flexedge::node::edge_listener::create(loops.loop(0),
            first_context, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), nullptr);
        const std::weak_ptr stopped_loop_weak(stopped_loop_listener);
        control_loops.stop();
        control_loops.join();
        loops.stop();
        loops.join();
        first_listener->request_stop();
        second_listener->request_stop();
        constrained_listener->request_stop();
        reusable_edge_listener->request_stop();
        bool restart_after_worker_retirement_rejected = false;
        try {
            first_listener->request_start();
        } catch (const std::runtime_error&) {
            restart_after_worker_retirement_rejected = true;
        }
        REQUIRE(restart_after_worker_retirement_rejected);
        stopped_loop_listener->request_stop();
        stopped_loop_listener.reset();
        REQUIRE(stopped_loop_weak.expired());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#undef REQUIRE
