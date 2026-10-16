#pragma once

#include <ruvia/http/http_status.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include "common/route_match.h"
#include "common/route_policy.h"
#include "node/data/access_log_capture.h"
#include "node/data/buffered_bytes_budget.h"
#include "node/data/health_supervisor.h"
#include "node/data/http1_request_reader.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_exchange.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_response_codec.h"
#include "node/data/origin_selection.h"
#include "node/data/response_compression.h"
#include "node/data/route_rules.h"
#include "node/data/worker_metrics.h"
#include "node/runtime/connected_clients.h"
#include "node/runtime/http_redirect.h"
#include "node/runtime/log_producer.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class tls_context_set;

// Worker-owned services shared by the sessions a listener admits on that worker.
struct http_session_context final {
    runtime_state& runtime_;
    origin_health_supervisor& health_;
    worker_metrics& metrics_;
    connected_clients& clients_;
    log_producer& logs_;
    origin_connection_pool& origin_connections_;
    buffered_bytes_budget& request_buffers_;
    buffered_bytes_budget& response_buffers_;
};

// One accepted client connection as its listener sees it, from accept until the session
// serving it is destroyed. close() reaches whichever stage currently owns the socket;
// destruction reports the connection retired. Worker loop only.
class edge_connection final {
public:
    explicit edge_connection(std::function<void()> on_retired)
        : on_retired_(std::move(on_retired)) {}

    edge_connection(const edge_connection&) = delete;
    edge_connection& operator=(const edge_connection&) = delete;

    ~edge_connection() {
        if (on_retired_) {
            on_retired_();
        }
    }

    void set_close(std::function<void()> close) {
        close_ = std::move(close);
    }

    void close() noexcept {
        if (close_) {
            close_();
        }
    }

    // The worker-local TLS runtime must outlive this connection, including reloads.
    void retain_tls_context(std::shared_ptr<tls_context_set> contexts) noexcept {
        tls_contexts_ = std::move(contexts);
    }

private:
    std::function<void()> close_;
    std::function<void()> on_retired_;
    std::shared_ptr<tls_context_set> tls_contexts_;
};

// Serves one HTTP/1 request on a client connection: routing, local responses, proxying
// with origin failover (streamed, or buffered for response compression), upgraded
// tunnels and the access log record.
template <typename client_stream_type>
class basic_http_session final
    : public std::enable_shared_from_this<basic_http_session<client_stream_type>> {
public:
    basic_http_session(client_stream_type stream, const http_session_context& context,
        std::uint64_t sequence, std::shared_ptr<edge_connection> connection, bool secure,
        std::string tls_fingerprint = {})
        : connection_(std::move(connection)),
          stream_(std::move(stream)),
          context_(context),
          timer_(stream_.get_executor()),
          access_log_(context_.logs_, secure, std::move(tls_fingerprint)),
          request_reservation_(context_.request_buffers_.lease()),
          sequence_(sequence),
          secure_(secure) {
        request_bytes_.reserve(8192);
        capture_client_address();
        context_.metrics_.connection_opened();
    }

    basic_http_session(const basic_http_session&) = delete;
    basic_http_session& operator=(const basic_http_session&) = delete;

    ~basic_http_session() {
        if (client_key_) {
            context_.clients_.closed(*client_key_);
        }
        context_.metrics_.connection_closed();
    }

    void start() {
        connection_->set_close([weak = this->weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->close();
            }
        });
        read();
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        ++origin_attempt_;
        cancel_timer();
        discard_exchange();
        std::error_code ignored;
        client_socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        ignored = client_socket().close(ignored);
    }

private:
    static constexpr std::size_t max_request_bytes = 2 * 1024 * 1024;

    enum class timeout_action { close,
        failover };

    static bool normal_io_close(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::error::operation_aborted ||
               error == asio::ssl::error::stream_truncated;
    }

    static bool origin_closed(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
    }

    void log(std::string_view message, std::string_view level = "warning") const noexcept {
        try {
            std::cerr << "flexedge node http1: " << message << '\n';
            context_.logs_.node(level, "http1", message);
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error) const noexcept {
        try {
            log(std::string(message) + ": " + error.what(), "error");
        } catch (...) {
        }
    }

    void log_io(std::string_view message, const std::error_code& error,
        std::string_view level = "error") const noexcept {
        if (normal_io_close(error)) {
            return;
        }
        try {
            log(std::string(message) + ": " + error.message(), level);
        } catch (...) {
        }
    }

    void log_client_io(std::string_view message, const std::error_code& error) const noexcept {
        const auto disconnected = error == asio::error::connection_reset ||
                                  error == asio::error::connection_aborted ||
                                  error == asio::error::broken_pipe;
        log_io(message, error, disconnected ? "info" : "error");
    }

    // Reads the peer once per connection: its text serves access logs and forwarding headers,
    // its key the worker's per-IP connection accounting until the session is destroyed.
    void capture_client_address() {
        std::error_code endpoint_error;
        const auto endpoint = client_socket().remote_endpoint(endpoint_error);
        if (endpoint_error) {
            client_address_ = "unknown";
            return;
        }
        std::error_code address_error;
        client_address_ = endpoint.address().to_string(address_error);
        if (address_error) {
            client_address_ = "unknown";
        }
        const auto key = connected_clients::get_key(endpoint.address());
        context_.clients_.opened(key);
        client_key_ = key;
    }

    void send(std::string bytes, std::uint32_t status_code, buffered_bytes_lease reservation = {}) {
        if (closed_ || response_pending_) {
            return;
        }
        response_pending_ = true;
        cancel_timer();
        discard_exchange();
        access_log_.response_head(bytes);
        response_reservation_ = std::move(reservation);
        response_bytes_ = std::move(bytes);
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(response_bytes_),
            [self, status_code](const std::error_code& error, std::size_t size) {
                self->context_.metrics_.traffic_out(size);
                self->access_log_.record(
                    error && self->access_log_.client_abort_enabled() ? 499 : status_code, size,
                    self->client_address_);
                if (error) {
                    self->log_client_io("client response write failed", error);
                }
                self->close();
            });
    }

    // A response generated by the edge itself.
    void reply(ruvia::http_status_code status, std::string_view extra_headers = {}) {
        std::string bytes = "HTTP/1.1 ";
        bytes.append(std::to_string(status.value()))
            .append(" ")
            .append(ruvia::http_reason_phrase(status))
            .append("\r\nContent-Length: 0\r\nConnection: close\r\n")
            .append(extra_headers)
            .append("\r\n");
        send(std::move(bytes), status.value());
    }

    void handle_request(const http1_request_reader& parsed) {
        const auto config = context_.runtime_.config();
        active_patterns_ = context_.runtime_.patterns();
        if (!config) {
            reply(ruvia::http_status::service_unavailable);
            return;
        }
        const auto host = parsed.request().header("Host");
        if (!host) {
            reply(ruvia::http_status::bad_request);
            return;
        }
        access_log_.request(parsed, *host);
        const auto hostname = flexedge::route_policy::hostname(*host);
        if (hostname.empty() || hostname.size() > 253) {
            log(hostname.empty() ? "request rejected: invalid HTTP Host header: empty hostname"
                                 : "request rejected: invalid HTTP Host header: hostname exceeds 253 bytes");
            reply(ruvia::http_status::bad_request);
            return;
        }
        const auto* website = config->website(hostname);
        if (website != nullptr) {
            access_log_.website(parsed, *website);
        }
        if (!secure_) {
            if (const auto redirect =
                    http_to_https_redirect(*config, hostname, parsed.request().target())) {
                reply(redirect->status_, "Location: " + redirect->location_ + "\r\n");
                return;
            }
        }
        if (website == nullptr) {
            reply(ruvia::http_status::misdirected_request);
            return;
        }
        hsts_enabled_ = secure_ && website->hsts_enabled();
        flexedge::route_policy::header_values match_headers;
        for (const auto& header : parsed.request().headers()) {
            match_headers.emplace_back(header.name(), header.value());
        }
        effective_route_ = evaluate_route_rules(*website, parsed.request().method(),
            parsed.request().target(), *host, match_headers, active_patterns_.get());
        if (!effective_route_) {
            reply(ruvia::http_status::bad_request);
            return;
        }
        route_ = &*effective_route_;
        if (route_->action() == "redirect") {
            redirect(*route_);
            return;
        }
        upgrade_requested_ = parsed.request().header("Upgrade").has_value();
        const auto consumed_bytes = parsed.message_bytes();
        if (upgrade_requested_ && consumed_bytes < request_bytes_.size()) {
            client_upgrade_remainder_.assign(request_bytes_.data() + consumed_bytes,
                request_bytes_.size() - consumed_bytes);
        }
        active_config_ = config;
        website_ = website;
        origins_ = origin_candidates(*website, sequence_, route_);
        const auto health = context_.health_.snapshot();
        std::stable_partition(origins_.begin(), origins_.end(), [&](const v2::Origin* origin) {
            return health->healthy(website->id(), origin->id());
        });
        healthy_threshold_ = website->healthy_threshold();
        unhealthy_threshold_ = website->unhealthy_threshold();
        if (origins_.empty()) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        proxy(parsed);
    }

    void redirect(const v2::RouteRule& route) {
        const auto& location = route.redirect_url();
        if (location.empty()) {
            reply(ruvia::http_status::bad_request);
            return;
        }
        std::string headers = "Location: " + location + "\r\n";
        for (const auto& header : route.response_headers()) {
            headers.append(header.name()).append(": ").append(header.value()).append("\r\n");
        }
        const auto status = flexedge::route_policy::redirect_status(route.redirect_status())
                                ? static_cast<std::uint16_t>(route.redirect_status())
                                : std::uint16_t{302};
        reply(ruvia::http_status_code::from_value(status), headers);
    }

    void proxy(const http1_request_reader& parsed) {
        if (client_address_ == "unknown") {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        const auto accept = parsed.request().header("Accept");
        force_identity_encoding_ = accept && accepts_event_stream(*accept);
        const auto accept_encoding = parsed.request().header("Accept-Encoding");
        buffered_ = website_->response_compression_enabled() && accept_encoding.has_value() &&
                    !upgrade_requested_ && !force_identity_encoding_;
        if (buffered_) {
            accept_encoding_ = *accept_encoding;
        }
        connect_timeout_ = std::chrono::seconds(website_->origin_connect_timeout_seconds());
        read_timeout_ = std::chrono::seconds(website_->origin_read_timeout_seconds());
        try_next_origin();
    }

    void arm(std::chrono::seconds timeout, std::uint64_t attempt, timeout_action action) {
        timer_.expires_after(timeout);
        const auto self = this->shared_from_this();
        timer_.async_wait([self, attempt, action](const std::error_code& error) {
            if (error || !self->origin_attempt_current(attempt)) {
                return;
            }
            self->log("origin attempt timed out");
            self->current_health_.failure(self->unhealthy_threshold_);
            if (action == timeout_action::failover) {
                self->try_next_origin();
            } else {
                self->close();
            }
        });
    }

    void cancel_timer() noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
    }

    void discard_exchange() noexcept {
        if (exchange_) {
            exchange_->close();
            exchange_.reset();
        }
    }

    [[nodiscard]] bool origin_attempt_current(std::uint64_t attempt) const noexcept {
        return !closed_ && !response_pending_ && attempt == origin_attempt_;
    }

    void origin_failed(std::uint64_t attempt) {
        if (origin_attempt_current(attempt)) {
            current_health_.failure(unhealthy_threshold_);
            try_next_origin();
        }
    }

    void try_next_origin() {
        ++origin_attempt_;
        cancel_timer();
        discard_exchange();
        if (origin_index_ >= origins_.size()) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        const auto& origin = *origins_[origin_index_++];
        current_health_ = context_.health_.target(website_->id(), origin.id());
        start_origin_attempt(origin, origin_attempt_);
    }

    // Each attempt prepares its own request so its response parser starts fresh.
    void start_origin_attempt(const v2::Origin& origin, std::uint64_t attempt) {
        auto prepared = prepare_origin_request(request_reader_, *website_,
            *request_reader_.request().header("Host"), client_address_, secure_,
            upgrade_requested_, false, force_identity_encoding_, route_,
            active_patterns_.get());
        if (!prepared) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        auto response =
            buffered_ ? origin_response_codec(std::move(prepared->response_exchange_), hsts_enabled_,
                            route_, context_.response_buffers_.lease())
                      : origin_response_codec(std::move(prepared->response_exchange_), hsts_enabled_,
                            route_);
        if (buffered_) {
            origin_target_ = std::move(prepared->target_);
        }
        exchange_ = std::make_shared<origin_exchange>(context_.origin_connections_,
            origin_connection_key{
                .host_ = origin.host(),
                .port_ = static_cast<std::uint16_t>(origin.port()),
                .secure_ = origin.protocol() == "https",
            },
            std::move(prepared->bytes_), std::move(response));
        arm(connect_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        exchange_->send([self, attempt](const std::error_code& error) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->log_io("origin request failed", error);
                self->origin_failed(attempt);
                return;
            }
            self->read_origin_head(attempt);
        });
    }

    void read_origin_head(std::uint64_t attempt) {
        arm(read_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        exchange_->read([self, attempt](const std::error_code& error, std::string_view bytes) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->log_io("origin response head read failed", error);
                self->origin_failed(attempt);
                return;
            }
            auto& response = self->exchange_->response();
            switch (response.consume_head(bytes)) {
                case origin_response_head_status::need_more:
                    self->read_origin_head(attempt);
                    return;
                case origin_response_head_status::failure:
                    self->log("origin response head parse failed");
                    self->origin_failed(attempt);
                    return;
                case origin_response_head_status::exhausted:
                    self->reply(ruvia::http_status::service_unavailable);
                    return;
                case origin_response_head_status::ready:
                    break;
            }
            self->current_health_.success(self->healthy_threshold_);
            if (self->buffered_) {
                self->continue_buffered(attempt);
                return;
            }
            self->tunnel_ = response.tunnel();
            self->relay_bytes_ = response.take_output();
            self->access_log_.response_head(self->relay_bytes_);
            self->relay_origin_bytes(attempt);
        });
    }

    void continue_buffered(std::uint64_t attempt) {
        if (exchange_->response().complete()) {
            send_buffered();
            return;
        }
        arm(read_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        exchange_->read([self, attempt](const std::error_code& error, std::string_view bytes) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            auto& response = self->exchange_->response();
            if (error) {
                if (origin_closed(error) && response.eof_completes_response()) {
                    self->send_buffered();
                    return;
                }
                self->log_io("buffered origin response read failed", error);
                self->origin_failed(attempt);
                return;
            }
            switch (response.consume_body(bytes)) {
                case origin_response_body_status::accepted:
                    self->continue_buffered(attempt);
                    return;
                case origin_response_body_status::failure:
                    self->log("origin response body parse failed");
                    self->origin_failed(attempt);
                    return;
                case origin_response_body_status::exhausted:
                    self->reply(ruvia::http_status::service_unavailable);
                    return;
            }
        });
    }

    void send_buffered() {
        cancel_timer();
        auto& codec = exchange_->response();
        auto response = codec.take_response();
        apply_response_compression(*website_, origin_target_, accept_encoding_, response);
        std::string wire;
        if (!codec.frame(response, wire)) {
            reply(ruvia::http_status::service_unavailable);
            return;
        }
        exchange_->release();
        send(std::move(wire), response.status_, std::move(response.reservation_));
    }

    void relay_origin_bytes(std::uint64_t attempt) {
        arm(read_timeout_, attempt, timeout_action::close);
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(relay_bytes_),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                self->context_.metrics_.traffic_out(size);
                self->proxied_response_bytes_ += size;
                if (error) {
                    self->access_log_.record(499, self->proxied_response_bytes_, self->client_address_);
                    self->log_client_io("client relay write failed", error);
                    self->close();
                    return;
                }
                auto& response = self->exchange_->response();
                if (self->tunnel_) {
                    self->access_log_.record(response.status_code().value_or(101),
                        self->proxied_response_bytes_, self->client_address_);
                    self->start_tunnel(attempt);
                } else if (response.complete()) {
                    self->access_log_.record(response.status_code().value_or(502),
                        self->proxied_response_bytes_, self->client_address_);
                    self->exchange_->release();
                    self->close();
                } else {
                    self->read_origin_body(attempt);
                }
            });
    }

    void read_origin_body(std::uint64_t attempt) {
        arm(read_timeout_, attempt, timeout_action::close);
        const auto self = this->shared_from_this();
        exchange_->read([self, attempt](const std::error_code& error, std::string_view bytes) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            auto& response = self->exchange_->response();
            if (origin_closed(error)) {
                self->access_log_.record(response.status_code().value_or(502),
                    self->proxied_response_bytes_, self->client_address_);
                self->close();
                return;
            }
            if (error) {
                self->log_io("origin response body read failed", error);
                self->close();
                return;
            }
            if (response.consume_body(bytes) != origin_response_body_status::accepted) {
                self->log("origin response body parse failed");
                self->close();
                return;
            }
            self->relay_bytes_.assign(bytes);
            self->relay_origin_bytes(attempt);
        });
    }

    void start_tunnel(std::uint64_t attempt) {
        exchange_->enter_tunnel();
        cancel_timer();
        if (client_upgrade_remainder_.empty()) {
            read_tunnel_client(attempt);
            read_tunnel_origin(attempt);
            return;
        }
        const auto self = this->shared_from_this();
        exchange_->write(asio::buffer(client_upgrade_remainder_),
            [self, attempt](const std::error_code& error) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                if (error) {
                    self->close();
                    return;
                }
                self->client_upgrade_remainder_.clear();
                self->read_tunnel_client(attempt);
                self->read_tunnel_origin(attempt);
            });
    }

    void read_tunnel_client(std::uint64_t attempt) {
        const auto self = this->shared_from_this();
        stream_.async_read_some(asio::buffer(client_tunnel_buffer_),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                if (error) {
                    self->close();
                    return;
                }
                self->exchange_->write(asio::buffer(self->client_tunnel_buffer_.data(), size),
                    [self, attempt](const std::error_code& write_error) {
                        if (!self->origin_attempt_current(attempt)) {
                            return;
                        }
                        if (write_error) {
                            self->close();
                            return;
                        }
                        self->read_tunnel_client(attempt);
                    });
            });
    }

    void read_tunnel_origin(std::uint64_t attempt) {
        const auto self = this->shared_from_this();
        exchange_->read([self, attempt](const std::error_code& error, std::string_view bytes) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            asio::async_write(self->stream_, asio::buffer(bytes),
                [self, attempt](const std::error_code& write_error, std::size_t size) {
                    if (!self->origin_attempt_current(attempt)) {
                        return;
                    }
                    self->context_.metrics_.traffic_out(size);
                    if (write_error) {
                        self->close();
                        return;
                    }
                    self->read_tunnel_origin(attempt);
                });
        });
    }

    void consume() noexcept {
        try {
            switch (request_reader_.consume(request_bytes_)) {
                case http1_request_read_status::complete:
                    handle_request(request_reader_);
                    return;
                case http1_request_read_status::bad_request:
                    reply(ruvia::http_status::bad_request);
                    return;
                case http1_request_read_status::too_large:
                    reply(ruvia::http_status::content_too_large);
                    return;
                case http1_request_read_status::need_more:
                    read();
                    return;
            }
        } catch (const std::exception& error) {
            log("request handling failed", error);
            close();
        } catch (...) {
            log("request handling failed with unknown exception", "error");
            close();
        }
    }

    void read() {
        if (closed_ || response_pending_) {
            return;
        }
        const auto self = this->shared_from_this();
        stream_.async_read_some(asio::buffer(read_buffer_),
            [self](const std::error_code& error, std::size_t size) {
                self->read_completed(error, size);
            });
    }

    void read_completed(const std::error_code& error, std::size_t size) noexcept {
        if (closed_ || response_pending_) {
            return;
        }
        if (error) {
            close();
            return;
        }
        try {
            if (!request_reservation_.try_grow(size)) {
                reply(ruvia::http_status::service_unavailable);
                return;
            }
            request_bytes_.append(read_buffer_.data(), size);
            consume();
        } catch (const std::exception& error) {
            log("client input handling failed", error);
            close();
        } catch (...) {
            log("client input handling failed with unknown exception", "error");
            close();
        }
    }

    [[nodiscard]] decltype(auto) client_socket() noexcept {
        return stream_.lowest_layer();
    }

    // Declared before the stream so state the stream depends on outlives it.
    std::shared_ptr<edge_connection> connection_;
    client_stream_type stream_;
    http_session_context context_;
    asio::steady_timer timer_;
    access_log_capture access_log_;
    http1_request_reader request_reader_{max_request_bytes};
    std::array<char, 8192> read_buffer_{};
    std::string request_bytes_;
    std::string response_bytes_;
    buffered_bytes_lease request_reservation_;
    buffered_bytes_lease response_reservation_;
    std::string client_address_;
    std::optional<connected_clients::address_key> client_key_;
    std::shared_ptr<const compiled_config> active_config_;
    std::shared_ptr<const route_patterns> active_patterns_;
    const v2::Website* website_{};
    std::optional<v2::RouteRule> effective_route_;
    const v2::RouteRule* route_{};
    std::vector<const v2::Origin*> origins_;
    std::size_t origin_index_{};
    std::uint64_t origin_attempt_{};
    origin_health_target current_health_;
    std::uint32_t healthy_threshold_{1};
    std::uint32_t unhealthy_threshold_{1};
    std::chrono::seconds connect_timeout_{10};
    std::chrono::seconds read_timeout_{30};
    std::shared_ptr<origin_exchange> exchange_;
    std::string origin_target_;
    std::string_view accept_encoding_;
    std::string relay_bytes_;
    std::string client_upgrade_remainder_;
    std::array<char, 16384> client_tunnel_buffer_{};
    std::uint64_t proxied_response_bytes_{};
    std::uint64_t sequence_{};
    bool secure_{};
    bool buffered_{};
    bool force_identity_encoding_{};
    bool upgrade_requested_{};
    bool tunnel_{};
    bool hsts_enabled_{};
    bool closed_{};
    bool response_pending_{};
};

}  // namespace flexedge::node
