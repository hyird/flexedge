#include "node/control/control_channel.h"

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/http/http1_server_request_parser.h>
#include <ruvia/http/websocket_handshake.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "common/file_digest.h"
#include "node/control/transport_config.h"
#include "node/runtime/loop_shutdown.h"
#include "node/runtime/secure_file.h"
#include "node/runtime/state_cipher.h"
#include "node/runtime/version.h"
#include "tests/support/config_fixture.h"

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

constexpr std::string_view node_id = "0123456789abcdef0123456789abcdef";
constexpr std::string_view node_secret = "synthetic-node-secret-0123456789abcdef";

void append_varint(std::string& output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

std::string length_delimited(std::uint32_t field, std::string_view bytes) {
    std::string result;
    append_varint(result, (static_cast<std::uint64_t>(field) << 3) | 2);
    append_varint(result, bytes.size());
    result.append(bytes);
    return result;
}

// Accepts the first TCP connection the control channel opens and closes it.
class control_listener final {
public:
    control_listener()
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
          thread_([this] { serve(); }) {}

    ~control_listener() {
        stopping_ = true;
        thread_.join();
    }

    control_listener(const control_listener&) = delete;
    control_listener& operator=(const control_listener&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!connected_) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

private:
    void serve() noexcept {
        std::error_code error;
        acceptor_.non_blocking(true, error);
        while (!stopping_ && !error) {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket, error);
            if (!error) {
                connected_ = true;
                return;
            }
            if (error == asio::error::would_block || error == asio::error::try_again) {
                error.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> stopping_{};
    std::atomic<bool> connected_{};
    std::thread thread_;
};

enum class session_behavior {
    unavailable,
    reject_authentication,
    reject_acknowledgement,
    violate_websocket_protocol,
    restart_after_heartbeat,
    disconnect_transport_after_heartbeat,
    delay_first_log_ack,
    lose_first_log_ack,
    reorder_log_ack,
    no_multiplexing,
    healthy,
};

// Exercises the real transport, authentication, acknowledgement and reconnect
// paths. Socket reads remain interruptible so a failed assertion cannot hang QA.
class channel_origin final {
public:
    channel_origin(flexedge::node::v2::ActiveState active, std::string binary_digest,
        std::vector<session_behavior> sessions)
        : active_(std::move(active)),
          binary_digest_(std::move(binary_digest)),
          sessions_(std::move(sessions)),
          acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
          thread_([this] { serve(); }) {}

    ~channel_origin() {
        stop();
    }

    void stop() {
        stopping_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    template <typename predicate_type>
    bool wait_until(predicate_type&& predicate, std::chrono::seconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate()) {
            if (failed_ || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    std::atomic<std::size_t> connections_{};
    std::atomic<std::size_t> heartbeats_{};
    std::atomic<std::size_t> batches_{};
    std::vector<flexedge::node::v2::Heartbeat> received_heartbeats_;
    std::vector<flexedge::node::v2::NodeLog> received_logs_;
    std::vector<flexedge::node::v2::ClientEnvelope::PayloadCase> received_order_;
    std::vector<std::string> delivered_batches_;
    std::atomic<std::size_t> heartbeats_while_logs_unacknowledged_{};
    std::exception_ptr failure_;

private:
    std::string read(asio::ip::tcp::socket& socket, std::size_t size) {
        std::string result(size, '\0');
        std::size_t offset{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (offset < size) {
            if (stopping_ || std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("test transport stopped or timed out");
            }
            std::error_code error;
            offset += socket.read_some(asio::buffer(result.data() + offset, size - offset), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else if (error) {
                throw std::system_error(error);
            }
        }
        return result;
    }

    void write(asio::ip::tcp::socket& socket, std::string_view bytes) {
        std::size_t offset{};
        while (offset < bytes.size() && !stopping_) {
            std::error_code error;
            offset += socket.write_some(asio::buffer(bytes.data() + offset, bytes.size() - offset), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            } else if (error) {
                throw std::system_error(error);
            }
        }
    }

    flexedge::node::v2::ClientEnvelope receive(asio::ip::tcp::socket& socket) {
        const auto head = read(socket, 2);
        REQUIRE((static_cast<unsigned char>(head[0]) & 0x0f) == 2);
        REQUIRE((static_cast<unsigned char>(head[1]) & 0x80) != 0);
        std::uint64_t size = static_cast<unsigned char>(head[1]) & 0x7f;
        if (size >= 126) {
            const auto extended = read(socket, size == 126 ? 2 : 8);
            size = 0;
            for (const unsigned char byte : extended) {
                size = size * 256 + byte;
            }
        }
        REQUIRE(size <= 1024 * 1024);
        const auto mask = read(socket, 4);
        auto payload = read(socket, static_cast<std::size_t>(size));
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] ^= mask[index % 4];
        }
        flexedge::node::v2::ClientEnvelope envelope;
        REQUIRE(envelope.ParseFromString(payload));
        return envelope;
    }

    void send(asio::ip::tcp::socket& socket, const flexedge::node::v2::ServerEnvelope& envelope) {
        const auto payload = flexedge::node::serialize_artifact(envelope);
        REQUIRE(payload.size() < 65536);
        std::string frame(1, static_cast<char>(0x82));
        if (payload.size() < 126) {
            frame.push_back(static_cast<char>(payload.size()));
        } else {
            frame.push_back(static_cast<char>(126));
            frame.push_back(static_cast<char>(payload.size() >> 8));
            frame.push_back(static_cast<char>(payload.size() & 0xff));
        }
        frame.append(payload);
        write(socket, frame);
    }

    void serve_session(asio::ip::tcp::socket& socket, session_behavior behavior) {
        socket.non_blocking(true);
        std::string request;
        while (!request.ends_with("\r\n\r\n")) {
            REQUIRE(request.size() < 16384);
            request.append(read(socket, 1));
        }
        if (behavior == session_behavior::unavailable) {
            write(socket, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            return;
        }
        ruvia::http1_server_request_parse_state parsed;
        ruvia::http1_server_request_parser().parse_head(request, parsed);
        REQUIRE(parsed.head_ready() != nullptr);
        const std::array<std::string_view, 1> subprotocols{flexedge::node::control_subprotocol};
        const auto handshake = ruvia::make_websocket_server_handshake(parsed.request_,
            {.supported_subprotocols_ = subprotocols});
        std::string response;
        handshake.for_each_response_part([&response](std::string_view part) { response.append(part); });
        write(socket, response);
        const auto authentication = receive(socket);
        REQUIRE(authentication.has_authenticate());
        REQUIRE(authentication.authenticate().multiplex_logs());
        flexedge::node::v2::ServerEnvelope welcome;
        welcome.set_request_id(authentication.request_id());
        if (behavior == session_behavior::reject_authentication) {
            welcome.mutable_error()->set_message("invalid node secret");
            send(socket, welcome);
            return;
        }
        auto* value = welcome.mutable_welcome();
        value->set_node_id(node_id);
        value->set_desired_node_spec_revision(active_.node_spec().content().revision());
        value->set_desired_release_id(active_.release().content().release_id());
        value->set_desired_manifest_digest(active_.release().digest_sha256());
        value->set_node_binary_sha256(binary_digest_);
        value->set_heartbeat_interval_seconds(1);
        value->set_multiplex_logs(behavior != session_behavior::no_multiplexing);
        send(socket, welcome);
        std::optional<flexedge::node::v2::ServerEnvelope> delayed_log_ack;
        std::optional<flexedge::node::v2::ServerEnvelope> delayed_heartbeat_ack;
        std::size_t log_batches{};
        for (;;) {
            const auto incoming = receive(socket);
            received_order_.push_back(incoming.payload_case());
            flexedge::node::v2::ServerEnvelope acknowledgement;
            acknowledgement.set_request_id(incoming.request_id());
            if (behavior == session_behavior::violate_websocket_protocol) {
                // Reserved opcode 3 is a real frame violation. Wait for the
                // client's Close so its protocol error is not hidden by EOF.
                write(socket, std::string_view("\x83\x00", 2));
                (void)read(socket, 1);
                return;
            }
            if (behavior == session_behavior::reject_acknowledgement) {
                send(socket, acknowledgement);
                return;
            }
            if (incoming.has_heartbeat()) {
                received_heartbeats_.push_back(incoming.heartbeat());
                auto* ack = acknowledgement.mutable_heartbeat_ack();
                ack->set_node_binary_sha256(binary_digest_);
                ack->set_next_interval_seconds(1);
                if (behavior == session_behavior::reorder_log_ack && !delayed_heartbeat_ack) {
                    delayed_heartbeat_ack = acknowledgement;
                } else {
                    send(socket, acknowledgement);
                }
                if (delayed_log_ack) {
                    ++heartbeats_while_logs_unacknowledged_;
                    if (behavior == session_behavior::delay_first_log_ack && heartbeats_while_logs_unacknowledged_ >= 2) {
                        send(socket, *delayed_log_ack);
                        delayed_log_ack.reset();
                    }
                }
                ++heartbeats_;
            } else if (incoming.has_release_probe()) {
                if (behavior == session_behavior::restart_after_heartbeat) {
                    write(socket, std::string_view("\x88\x02\x03\xe9", 4));
                    return;
                }
                if (behavior == session_behavior::disconnect_transport_after_heartbeat) {
                    // A restarted process can close TCP without an RFC 6455 Close.
                    return;
                }
                const auto replies = flexedge::node::release_probe_replies(incoming.request_id(),
                    binary_digest_, active_.node_spec().content().revision(),
                    active_.release().content().release_id(), active_.release().digest_sha256(), {});
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                send(socket, replies.front());
            } else {
                REQUIRE(incoming.has_log_delivery());
                acknowledgement.mutable_log_delivery_ack();
                delivered_batches_.push_back(incoming.log_delivery().SerializeAsString());
                ++log_batches;
                if (log_batches == 1 && (behavior == session_behavior::delay_first_log_ack || behavior == session_behavior::lose_first_log_ack)) {
                    delayed_log_ack = acknowledgement;
                } else {
                    send(socket, acknowledgement);
                    delayed_log_ack.reset();
                }
                if (delayed_heartbeat_ack) {
                    send(socket, *delayed_heartbeat_ack);
                    delayed_heartbeat_ack.reset();
                }
                for (const auto& event : incoming.log_delivery().events()) {
                    if (event.has_node_log()) {
                        received_logs_.push_back(event.node_log());
                    }
                }
                ++batches_;
            }
        }
    }

    void serve() noexcept {
        try {
            acceptor_.non_blocking(true);
            while (!stopping_) {
                asio::ip::tcp::socket socket(io_);
                std::error_code error;
                acceptor_.accept(socket, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                if (error) {
                    throw std::system_error(error);
                }
                const auto index = connections_.fetch_add(1);
                const auto behavior = index < sessions_.size() ? sessions_[index] : session_behavior::healthy;
                try {
                    serve_session(socket, behavior);
                } catch (const std::system_error& socket_error) {
                    if (socket_error.code() != asio::error::eof &&
                        socket_error.code() != asio::error::connection_reset) {
                        throw;
                    }
                }
            }
        } catch (...) {
            if (!stopping_) {
                failure_ = std::current_exception();
                failed_ = true;
            }
        }
    }

    flexedge::node::v2::ActiveState active_;
    std::string binary_digest_;
    std::vector<session_behavior> sessions_;
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> stopping_{};
    std::atomic<bool> failed_{};
    std::thread thread_;
};

struct node_start final {
    bool connected_{};
    std::int64_t applied_node_spec_revision_{};
    std::vector<std::string> state_logs_;
};

// Starts the real control channel over a state directory and reports whether it reached
// the control plane transport.
node_start start_node(const std::filesystem::path& root, const std::filesystem::path& state) {
    const auto credentials_path = root / "credentials";
    flexedge::node::write_secure_file_atomic(credentials_path,
        "node_id=" + std::string(node_id) + "\nsecret=" + std::string(node_secret) + "\n");
    auto credentials = flexedge::node::node_credentials::load(credentials_path);
    const auto binary_path = root / "node";
    flexedge::node::write_secure_file_atomic(binary_path, "synthetic node binary");

    node_start result;
    ruvia::event_loop_pool workers({.loop_count_ = 1});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    flexedge::node::runtime_state runtime;
    auto [queued_sender, queued_receiver] = ruvia::make_channel<bool>(control.loop(0).handle(), {.capacity_ = 1});
    flexedge::node::node_log_buffer logs(1, [queued_sender] { (void)queued_sender.send(true); });
    {
        flexedge::node::data_plane plane(workers, control.loop(0), runtime, logs);
        flexedge::node::self_updater updater({
            .binary_path_ = binary_path,
            .current_version_ = std::string(flexedge::node::node_version),
            .upgrade_record_path_ = root / "pending-upgrade",
            .node_log_ = [](std::string_view, std::string_view, std::string_view) { return true; },
            .request_restart_ = [] {},
        });
        control_listener listener;
        ruvia::stop_source stop_source;
        flexedge::node::control_channel channel(control.loop(0),
            {
                .web_socket_ = flexedge::node::control_transport_config(
                    "ws://127.0.0.1:" + std::to_string(listener.port())),
                .stop_token_ = stop_source.token(),
            },
            flexedge::node::state_store{state, credentials.secret()}, credentials, runtime, plane,
            logs, updater, std::move(queued_receiver));
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        auto task = control.loop(0).start(channel.run());
        result.connected_ = listener.wait_connected(std::chrono::seconds(5));
        stop_source.request_stop();
        task.get();
        result.applied_node_spec_revision_ = runtime.applied_node_spec_revision();
        shutdown.stop_and_join();
        plane.request_stop_accepting();
    }
    if (const auto delivery = logs.take(node_id); delivery) {
        for (const auto& event : delivery->value().events()) {
            if (event.has_node_log() && event.node_log().category() == "state") {
                result.state_logs_.push_back(event.node_log().message());
            }
        }
    }
    return result;
}

// A listener-free configuration the data plane activates without binding ports.
flexedge::testing::TestConfig offline_config() {
    auto config = flexedge::testing::snapshot(false);
    config.active.mutable_node_spec()->mutable_content()->set_enabled(false);
    config.mutableWebsite()->set_https_enabled(false);
    for (auto& domain : *config.mutableWebsite()->mutable_domains()) {
        domain.set_https_enabled(false);
    }
    config.finalize();
    return config;
}

bool logged(const node_start& start, std::string_view text) {
    for (const auto& message : start.state_logs_) {
        if (message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<flexedge::node::v2::NodeLog> drain_node_logs(flexedge::node::node_log_buffer& logs) {
    std::vector<flexedge::node::v2::NodeLog> result;
    while (auto delivery = logs.take(node_id)) {
        for (const auto& event : delivery->value().events()) {
            if (event.has_node_log()) {
                result.push_back(event.node_log());
            }
        }
        logs.acknowledge(std::move(*delivery));
    }
    return result;
}

std::size_t count_logs(const std::vector<flexedge::node::v2::NodeLog>& logs,
    std::string_view category, std::string_view level, std::string_view message = {}) {
    return static_cast<std::size_t>(std::count_if(logs.begin(), logs.end(), [&](const auto& entry) {
        return entry.category() == category && entry.level() == level &&
               (message.empty() || entry.message() == message);
    }));
}

void verify_channel_log_policy() {
    using namespace flexedge::node;
    using code_type = ruvia::websocket_client_error::code_type;
    for (const auto code : {code_type::timeout, code_type::cancelled, code_type::resolve_failed,
             code_type::connect_failed, code_type::handshake_rejected, code_type::io_error, code_type::closing}) {
        REQUIRE(classify_channel_failure(code) == channel_failure_kind::transient);
    }
    for (const auto code : {code_type::invalid_config, code_type::invalid_state, code_type::tls_failed,
             code_type::protocol_error, code_type::message_too_large}) {
        REQUIRE(classify_channel_failure(code) == channel_failure_kind::permanent);
    }
    node_log_buffer logs;
    channel_log log(logs, "control");
    log.recovered();
    REQUIRE(!logs.pending());
    log.failed("connection refused", channel_failure_kind::transient);
    REQUIRE(log.retry_delay() == std::chrono::seconds(1));
    log.failed("connection refused", channel_failure_kind::transient);
    REQUIRE(log.retry_delay() == std::chrono::seconds(2));
    log.failed("connection refused", channel_failure_kind::transient);
    REQUIRE(log.retry_delay() == std::chrono::seconds(4));
    for (int attempt = 0; attempt < 10; ++attempt) {
        log.failed("temporary failure", channel_failure_kind::transient);
    }
    REQUIRE(log.retry_delay() == std::chrono::seconds(30));
    log.recovered();
    log.recovered();
    REQUIRE(log.retry_delay() == std::chrono::seconds(1));
    log.failed("invalid credentials", channel_failure_kind::permanent);
    log.recovered();
    const auto entries = drain_node_logs(logs);
    REQUIRE(count_logs(entries, "control", "warning") == 13);
    REQUIRE(count_logs(entries, "control", "error") == 1);
    REQUIRE(count_logs(entries, "control", "info", "connection restored") == 2);
    log.failed("a new outage", channel_failure_kind::transient);
    REQUIRE(log.retry_delay() == std::chrono::seconds(1));
    log.failed(ruvia::websocket_client_error(code_type::protocol_error,
        "WebSocket transport ended before peer Close"));
    log.failed(ruvia::websocket_client_error(code_type::protocol_error,
        "WebSocket peer violated the protocol"));
    log.failed(ruvia::websocket_client_error(code_type::protocol_error,
        "WebSocket transport ended before peer Close with invalid frame"));
    log.failed(ruvia::websocket_client_error(code_type::invalid_config,
        "WebSocket transport ended before peer Close"));
    const auto next_outage = drain_node_logs(logs);
    REQUIRE(count_logs(next_outage, "control", "warning") == 2);
    REQUIRE(count_logs(next_outage, "control", "error") == 3);
}

void verify_control_reconnect(const std::filesystem::path& root,
    std::vector<session_behavior> sessions, std::size_t expected_heartbeats,
    std::size_t expected_warnings, std::size_t expected_errors, std::size_t expected_recoveries) {
    auto config = offline_config();
    config.active.mutable_node_spec()->mutable_content()->set_node_id(node_id);
    config.finalize();
    const auto state = root / ("reconnect-state-" + std::to_string(static_cast<int>(sessions.front())));
    const flexedge::node::state_store store(state, node_secret);
    store.stage(config.active, config.objects);
    store.activate_staged();
    auto credentials = flexedge::node::node_credentials::load(root / "credentials");
    const auto binary = root / "node";
    const auto expected_connections = sessions.size();
    channel_origin origin(config.active, flexedge::crypto::file_sha256(binary), std::move(sessions));
    ruvia::event_loop_pool workers({.loop_count_ = 1});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    flexedge::node::runtime_state runtime;
    auto [queued_sender, queued_receiver] = ruvia::make_channel<bool>(control.loop(0).handle(), {.capacity_ = 1});
    flexedge::node::node_log_buffer logs(1, [queued_sender] { (void)queued_sender.send(true); });
    {
        flexedge::node::data_plane plane(workers, control.loop(0), runtime, logs);
        flexedge::node::self_updater updater({
            .binary_path_ = binary,
            .current_version_ = std::string(flexedge::node::node_version),
            .upgrade_record_path_ = root / "pending-upgrade",
            .node_log_ = [](std::string_view, std::string_view, std::string_view) { return true; },
            .request_restart_ = [] {},
        });
        ruvia::stop_source stop_source;
        flexedge::node::control_channel channel(control.loop(0),
            {.web_socket_ = flexedge::node::control_transport_config(
                 "ws://127.0.0.1:" + std::to_string(origin.port())),
                .stop_token_ = stop_source.token()},
            store, credentials, runtime, plane, logs, updater, std::move(queued_receiver));
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        auto task = control.loop(0).start(channel.run());
        const auto connected = origin.wait_until([&] { return origin.connections_.load() >= expected_connections && origin.heartbeats_.load() >= expected_heartbeats; },
            std::chrono::seconds(15));
        // Let the client consume the acknowledgement before cancelling its next read.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop_source.request_stop();
        task.get();
        shutdown.stop_and_join();
        plane.request_stop_accepting();
        origin.stop();
        if (origin.failure_) {
            std::rethrow_exception(origin.failure_);
        }
        REQUIRE(connected);
    }
    auto entries = drain_node_logs(logs);
    entries.insert(entries.end(), origin.received_logs_.begin(), origin.received_logs_.end());
    REQUIRE(origin.connections_ == expected_connections);
    REQUIRE(origin.received_heartbeats_.size() >= expected_heartbeats);
    for (const auto& heartbeat : origin.received_heartbeats_) {
        REQUIRE(heartbeat.health() == "healthy");
        REQUIRE(heartbeat.last_error().empty());
    }
    REQUIRE(count_logs(entries, "control", "warning") == expected_warnings);
    REQUIRE(count_logs(entries, "control", "error") == expected_errors);
    REQUIRE(count_logs(entries, "control", "info", "connection restored") == expected_recoveries);
    REQUIRE(entries.size() == expected_warnings + expected_errors + expected_recoveries);
}

void verify_multiplexed_logs(const std::filesystem::path& root, bool retry, bool cancel = false) {
    auto config = offline_config();
    config.active.mutable_node_spec()->mutable_content()->set_node_id(node_id);
    config.finalize();
    const flexedge::node::state_store store(root / (cancel ? "multiplex-cancel" : retry ? "multiplex-retry"
                                                                                        : "multiplex-normal"),
        node_secret);
    store.stage(config.active, config.objects);
    store.activate_staged();
    channel_origin origin(config.active, flexedge::crypto::file_sha256(root / "node"),
        retry || cancel ? std::vector{session_behavior::lose_first_log_ack} : std::vector{session_behavior::delay_first_log_ack});
    auto credentials = flexedge::node::node_credentials::load(root / "credentials");
    ruvia::event_loop_pool workers({.loop_count_ = 1});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    const auto loop = control.loop(0);
    auto [queued_sender, queued_receiver] = ruvia::make_channel<bool>(loop.handle(), {.capacity_ = 1});
    flexedge::node::node_log_buffer logs(1, [queued_sender] { (void)queued_sender.send(true); });
    logs.node("info", "startup", "initial event");
    ruvia::stop_source stop_source;
    flexedge::node::runtime_state runtime;
    flexedge::node::data_plane plane(workers, loop, runtime, logs);
    flexedge::node::self_updater updater({
        .binary_path_ = root / "node",
        .current_version_ = std::string(flexedge::node::node_version),
        .upgrade_record_path_ = root / "pending-upgrade",
        .node_log_ = [](std::string_view, std::string_view, std::string_view) { return true; },
        .request_restart_ = [] {},
    });
    flexedge::node::control_channel channel(loop,
        {.web_socket_ = flexedge::node::control_transport_config(
             "ws://127.0.0.1:" + std::to_string(origin.port())),
            .stop_token_ = stop_source.token()},
        store, credentials, runtime, plane, logs, updater, std::move(queued_receiver));
    flexedge::node::loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    auto task = loop.start(channel.run());
    const auto delivered = origin.wait_until([&] {
        return cancel ? origin.batches_.load() >= 1 && origin.heartbeats_.load() >= 2 : origin.batches_.load() >= (retry ? 2 : 1) && logs.retained_events() == 0;
    },
        std::chrono::seconds(12));
    // A normal drained batch must leave the channel idle, including after recovery.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto stopped_at = std::chrono::steady_clock::now();
    stop_source.request_stop();
    task.get();
    REQUIRE(std::chrono::steady_clock::now() - stopped_at < std::chrono::seconds(2));
    shutdown.stop_and_join();
    origin.stop();
    if (origin.failure_) {
        std::rethrow_exception(origin.failure_);
    }
    REQUIRE(delivered);
    if (cancel) {
        REQUIRE(logs.retained_events() > 0);
        REQUIRE(logs.take(node_id).has_value());
        REQUIRE(origin.connections_ == 1);
        return;
    }
    REQUIRE(logs.retained_events() == 0);
    REQUIRE(origin.connections_ == 1);
    REQUIRE(origin.heartbeats_while_logs_unacknowledged_ >= 2);
    REQUIRE(!origin.received_order_.empty() && origin.received_order_.front() == flexedge::node::v2::ClientEnvelope::kHeartbeat);
    if (retry) {
        REQUIRE(origin.delivered_batches_.size() >= 2);
        REQUIRE(origin.delivered_batches_[0] == origin.delivered_batches_[1]);
    }
    REQUIRE(count_logs(origin.received_logs_, "log", "warning") == (retry ? 1 : 0));
    REQUIRE(count_logs(origin.received_logs_, "log", "error") == 0);
    REQUIRE(count_logs(origin.received_logs_, "log", "info", "connection restored") == (retry ? 1 : 0));
    REQUIRE(std::none_of(origin.received_logs_.begin(), origin.received_logs_.end(),
        [](const auto& entry) { return entry.category() == "log-ingest"; }));
}

ruvia::task<void> verify_priority_session(ruvia::websocket_client& client) {
    co_await client.connect();
    const auto connection = client.handle();
    flexedge::node::v2::ClientEnvelope authentication;
    authentication.set_request_id("authenticate");
    auto* identity = authentication.mutable_authenticate();
    identity->set_node_id(node_id);
    identity->set_secret(node_secret);
    identity->set_multiplex_logs(true);
    const auto welcome = co_await flexedge::node::authenticate_control_envelope(connection,
        std::move(authentication), "priority fixture did not authenticate");
    REQUIRE(welcome.multiplex_logs());
    const auto worker = client.worker();
    flexedge::node::control_stream stream(worker, connection, {});
    std::exception_ptr failure;
    try {
        flexedge::node::v2::ClientEnvelope logs;
        logs.mutable_log_delivery()->set_node_id(node_id);
        auto log_reply = stream.send(logs, flexedge::node::control_request_priority::logs);
        flexedge::node::v2::ClientEnvelope heartbeat;
        heartbeat.mutable_heartbeat()->set_node_id(node_id);
        auto heartbeat_reply = stream.send(heartbeat);
        REQUIRE(logs.request_id() != heartbeat.request_id());
        stream.start();
        // The mock acknowledges the log first, although the control request
        // was queued second and must be written first on this one socket.
        const auto heartbeat_ack = co_await heartbeat_reply.receive(std::chrono::seconds(3));
        const auto log_ack = co_await log_reply.receive(std::chrono::seconds(3));
        REQUIRE(heartbeat_ack.has_heartbeat_ack() && heartbeat_ack.request_id() == heartbeat.request_id());
        REQUIRE(log_ack.has_log_delivery_ack() && log_ack.request_id() == logs.request_id());
    } catch (...) {
        failure = std::current_exception();
    }
    co_await stream.shutdown();
    co_await client.shutdown();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void verify_multiplexed_priority(const std::filesystem::path& root) {
    const auto config = offline_config();
    channel_origin origin(config.active, flexedge::crypto::file_sha256(root / "node"),
        {session_behavior::reorder_log_ack});
    ruvia::event_loop_pool loops({.loop_count_ = 1});
    const auto loop = loops.loop(0);
    ruvia::websocket_client client(loop, flexedge::node::control_transport_config(
                                             "ws://127.0.0.1:" + std::to_string(origin.port())));
    loops.start();
    loop.start(verify_priority_session(client)).get();
    loops.stop();
    loops.join();
    origin.stop();
    if (origin.failure_) {
        std::rethrow_exception(origin.failure_);
    }
    REQUIRE(origin.connections_ == 1);
    REQUIRE(origin.received_order_.size() == 2);
    REQUIRE(origin.received_order_[0] == flexedge::node::v2::ClientEnvelope::kHeartbeat);
    REQUIRE(origin.received_order_[1] == flexedge::node::v2::ClientEnvelope::kLogDelivery);
}

template <typename function_type>
bool rejects_as_unusable(function_type&& function) {
    try {
        function();
    } catch (const flexedge::node::stored_state_unusable&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("flexedge-control-channel-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct directory_cleanup final {
        std::filesystem::path path_;
        ~directory_cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    } cleanup{root};

    try {
        verify_channel_log_policy();
        // A usable persisted state is served before the control plane is reached.
        {
            const auto state = root / "valid";
            const flexedge::node::state_store store(state, node_secret);
            const auto config = offline_config();
            store.stage(config.active, config.objects);
            store.activate_staged();
            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 3);
            REQUIRE(start.state_logs_.empty());
            REQUIRE(std::filesystem::exists(state / "active" / "state.pb"));
            REQUIRE(store.load().active_.SerializeAsString() == config.active.SerializeAsString());
        }

        // An object persisted by another code line carries a field this build does not know.
        // Re-serialization moves the unknown field behind the website, so the digest differs.
        {
            const auto state = root / "unknown-field";
            const flexedge::node::state_store store(state, node_secret);
            auto config = offline_config();
            const auto content =
                length_delimited(3, "cache-policy") +
                length_delimited(10, flexedge::node::serialize_artifact(config.website()));
            const auto digest = flexedge::node::artifact_digest(std::string_view(content));
            std::vector<flexedge::node::v2::DeliveryObject> certificates;
            for (const auto& object : config.objects) {
                if (object.content().has_certificate()) {
                    certificates.push_back(object);
                }
            }
            auto* manifest = config.active.mutable_release()->mutable_content();
            for (auto& reference : *manifest->mutable_objects()) {
                if (reference.kind() == flexedge::node::v2::OBJECT_KIND_WEBSITE) {
                    reference.set_digest_sha256(digest);
                }
            }
            config.active.mutable_release()->set_digest_sha256(
                flexedge::node::artifact_digest(*manifest));
            store.stage(config.active, certificates);
            store.activate_staged();
            flexedge::node::write_secure_file_atomic(state / "objects" / (digest + ".pb"),
                flexedge::node::state_cipher(node_secret)
                    .seal(length_delimited(1, digest) + length_delimited(2, content)));
            REQUIRE(rejects_as_unusable([&] { (void)store.load_object(digest); }));
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 0);
            REQUIRE(logged(start, "failed content verification"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
            REQUIRE(std::filesystem::exists(state / "previous" / "state.pb"));
            REQUIRE(!store.load().active_.has_node_spec());

            // Only the local copy is discarded; it is fetched again under the same name.
            store.discard_object(digest);
            REQUIRE(!std::filesystem::exists(state / "objects" / (digest + ".pb")));
            REQUIRE(!store.load_object(digest));
        }

        // Tampered ciphertext no longer authenticates.
        {
            const auto state = root / "tampered";
            const flexedge::node::state_store store(state, node_secret);
            const auto config = offline_config();
            store.stage(config.active, config.objects);
            store.activate_staged();
            auto sealed = *flexedge::node::read_secure_file(state / "active" / "state.pb");
            sealed.back() = static_cast<char>(sealed.back() ^ 0x01);
            flexedge::node::write_secure_file_atomic(state / "active" / "state.pb", sealed);
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 0);
            REQUIRE(logged(start, "state.pb: stored node state authentication failed"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
            // The discarded copy replaces the previous slot instead of accumulating.
            REQUIRE(*flexedge::node::read_secure_file(state / "previous" / "state.pb") == sealed);
        }

        // Authentic bytes that are not an active state protobuf.
        {
            const auto state = root / "not-protobuf";
            const flexedge::node::state_store store(state, node_secret);
            flexedge::node::write_secure_file_atomic(state / "active" / "state.pb",
                flexedge::node::state_cipher(node_secret).seal(std::string("\xff\xff\xff", 3)));
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(logged(start, "stored active state is not valid protobuf"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
        }

        // Control plane input keeps its checks: objects must reproduce their digest to be
        // staged, and malformed digests are not treated as a local cache problem.
        {
            const flexedge::node::state_store store(root / "boundary", node_secret);
            auto config = offline_config();
            config.objects.front().set_digest_sha256(std::string(64, 'a'));
            bool rejected = false;
            try {
                store.stage(config.active, config.objects);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            REQUIRE(rejected);
            bool invalid_digest_rejected = false;
            try {
                (void)store.load_object("not-a-digest");
            } catch (const flexedge::node::stored_state_unusable&) {
            } catch (const std::runtime_error&) {
                invalid_digest_rejected = true;
            }
            REQUIRE(invalid_digest_rejected);
        }
        verify_control_reconnect(root,
            {session_behavior::unavailable, session_behavior::reject_authentication,
                session_behavior::reject_acknowledgement, session_behavior::restart_after_heartbeat,
                session_behavior::healthy},
            2, 2, 2, 2);
        verify_control_reconnect(root,
            {session_behavior::disconnect_transport_after_heartbeat, session_behavior::healthy},
            2, 1, 0, 1);
        verify_control_reconnect(root,
            {session_behavior::violate_websocket_protocol, session_behavior::healthy},
            1, 0, 1, 1);
        verify_control_reconnect(root, {session_behavior::no_multiplexing}, 0, 0, 1, 0);
        verify_multiplexed_priority(root);
        verify_multiplexed_logs(root, false);
        verify_multiplexed_logs(root, true);
        verify_multiplexed_logs(root, false, true);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
