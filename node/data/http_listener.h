#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_signal.h>
#include <ruvia/http/http_header.h>
#include <ruvia/http/http_status.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#if defined(SO_REUSEPORT) && !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "node/data/buffered_bytes_budget.h"
#include "node/data/buffered_origin_exchange.h"
#include "node/data/http1_request_reader.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_health.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_response_codec.h"
#include "node/data/origin_selection.h"
#include "node/data/origin_tls.h"
#include "node/data/origin_transport.h"
#include "node/data/response_compression.h"
#include "node/runtime/connected_clients.h"
#include "node/runtime/http_redirect.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_metrics.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class listener_activation_gate final {
public:
    void activate() noexcept {
        active_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool active() const noexcept {
        return active_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> active_{};
};

template <typename client_stream_type>
class basic_http_session final : public std::enable_shared_from_this<basic_http_session<client_stream_type>> {
public:
    basic_http_session(client_stream_type stream, runtime_state& runtime, origin_health_registry& health,
        runtime_metrics& metrics, connected_clients& clients, node_log_buffer::producer& logs,
        origin_connection_pool& origin_connections, buffered_bytes_budget& request_buffers,
        buffered_bytes_budget& response_buffers, std::uint64_t sequence,
        std::shared_ptr<const void> transport_state = nullptr, bool secure = false,
        std::string tls_fingerprint = {}, std::function<void()> on_retired = {})
        : stream_(std::move(stream)),
          origin_connections_(origin_connections),
          timer_(stream_.get_executor()),
          runtime_(runtime),
          health_(health),
          metrics_(metrics),
          clients_(clients),
          logs_(logs),
          response_buffers_(response_buffers),
          request_reservation_(request_buffers.lease()),
          tls_fingerprint_(std::move(tls_fingerprint)),
          sequence_(sequence),
          transport_state_(std::move(transport_state)),
          secure_(secure),
          on_retired_(std::move(on_retired)) {
        request_bytes_.reserve(8192);
        capture_client_address();
        metrics_.connection_opened();
    }

    ~basic_http_session() {
        if (client_key_) {
            clients_.closed(*client_key_);
        }
        metrics_.connection_closed();
        if (on_retired_) {
            on_retired_();
        }
    }

    void start() {
        read();
    }

private:
    static constexpr std::size_t max_request_bytes = 2 * 1024 * 1024;

    enum class timeout_action { close,
        failover };

    static std::string response(std::string_view status, std::string_view extra_headers = {}) {
        std::string result = "HTTP/1.1 ";
        result.append(status);
        result.append("\r\nContent-Length: 0\r\nConnection: close\r\n");
        result.append(extra_headers);
        result.append("\r\n");
        return result;
    }

    static bool should_forward_buffered_response_header(std::string_view name,
        bool hsts_enabled) noexcept {
        return !http_header_name(name, "Content-Length") &&
               !http_header_name(name, "Transfer-Encoding") && !http_header_name(name, "Connection") &&
               !(hsts_enabled && http_header_name(name, "Strict-Transport-Security"));
    }

    static bool normal_io_close(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::error::operation_aborted ||
               error == asio::ssl::error::stream_truncated;
    }

    void log(std::string_view message) const noexcept {
        try {
            std::cerr << "flexedge node http1: " << message << '\n';
            logs_.node("warning", "http1", message);
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error,
        std::string_view level = "error") const noexcept {
        try {
            const auto text = std::string(message) + ": " + error.what();
            std::cerr << "flexedge node http1: " << text << '\n';
            logs_.node(level, "http1", text);
        } catch (...) {
        }
    }

    void log_io(std::string_view message, const std::error_code& error,
        std::string_view level = "error") const noexcept {
        if (normal_io_close(error)) {
            return;
        }
        try {
            const auto text = std::string(message) + ": " + error.message();
            std::cerr << "flexedge node http1: " << text << '\n';
            logs_.node(level, "http1", text);
        } catch (...) {
        }
    }

    void log_client_io(std::string_view message, const std::error_code& error) const noexcept {
        const auto disconnected = error == asio::error::connection_reset ||
                                  error == asio::error::connection_aborted ||
                                  error == asio::error::broken_pipe;
        log_io(message, error, disconnected ? "info" : "error");
    }

    static std::uint32_t wire_status_code(std::string_view bytes) noexcept {
        if (!bytes.starts_with("HTTP/1.1 ") || bytes.size() < 12) {
            return 0;
        }
        const auto a = bytes[9];
        const auto b = bytes[10];
        const auto c = bytes[11];
        if (!std::isdigit(static_cast<unsigned char>(a)) ||
            !std::isdigit(static_cast<unsigned char>(b)) ||
            !std::isdigit(static_cast<unsigned char>(c))) {
            return 0;
        }
        return static_cast<std::uint32_t>((a - '0') * 100 + (b - '0') * 10 + (c - '0'));
    }

    static void append_bounded(std::string& result, std::string_view value, std::size_t maximum) {
        if (result.size() >= maximum) {
            return;
        }
        const auto available = maximum - result.size();
        result.append(value.substr(0, available));
    }

    static std::string request_header_block(const http1_request_reader& parsed) {
        std::string result;
        for (const auto& header : parsed.request().headers()) {
            append_bounded(result, header.name(), node_log_buffer::max_request_headers_bytes);
            append_bounded(result, ": ", node_log_buffer::max_request_headers_bytes);
            append_bounded(result, node_log_buffer::request_header_value(header.name(), header.value()),
                node_log_buffer::max_request_headers_bytes);
            append_bounded(result, "\n", node_log_buffer::max_request_headers_bytes);
            if (result.size() >= node_log_buffer::max_request_headers_bytes) {
                break;
            }
        }
        return result;
    }

    static std::string header_block_for_log(std::string_view block, std::size_t maximum) {
        std::string result;
        while (!block.empty() && result.size() < maximum) {
            const auto line_end = block.find("\r\n");
            const auto line = line_end == std::string_view::npos ? block : block.substr(0, line_end);
            const auto colon = line.find(':');
            if (colon == std::string_view::npos) {
                append_bounded(result, line, maximum);
            } else {
                auto value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                const auto name = line.substr(0, colon);
                append_bounded(result, name, maximum);
                append_bounded(result, ": ", maximum);
                append_bounded(result, node_log_buffer::response_header_value(name, value), maximum);
            }
            append_bounded(result, "\n", maximum);
            if (line_end == std::string_view::npos) {
                break;
            }
            block.remove_prefix(line_end + 2);
        }
        return result;
    }

    static std::pair<std::string, bool> request_body_for_log(const http1_request_reader& parsed) {
        std::string body;
        if (parsed.has_body()) {
            const auto framed = parsed.body();
            if (!framed) {
                return {};
            }
            body.assign(*framed);
        }
        const auto truncated = body.size() > node_log_buffer::max_request_body_bytes;
        if (truncated) {
            body.resize(node_log_buffer::max_request_body_bytes);
        }
        return {node_log_buffer::request_body_value(body), truncated};
    }

    static std::string_view http1_response_headers(std::string_view bytes) noexcept {
        const auto head_end = bytes.find("\r\n\r\n");
        const auto status_line_end = bytes.find("\r\n");
        if (head_end == std::string_view::npos || status_line_end == std::string_view::npos ||
            status_line_end + 2 > head_end) {
            return {};
        }
        return bytes.substr(status_line_end + 2, head_end - status_line_end - 2);
    }

    static std::string query_string_for_log(std::string_view target) {
        const auto question = target.find('?');
        if (question == std::string_view::npos) {
            return {};
        }
        const auto fragment = target.find('#', question + 1);
        return std::string(target.substr(question + 1, fragment == std::string_view::npos
                                                           ? std::string_view::npos
                                                           : fragment - question - 1));
    }

    static void apply_status_code_ranges(std::array<bool, 5>& target, const v2::Website& website) {
        if (website.access_log_status_code_ranges().empty()) {
            target.fill(true);
            return;
        }
        target.fill(false);
        for (const auto& value : website.access_log_status_code_ranges()) {
            if (value == "1xx") {
                target[0] = true;
            } else if (value == "2xx") {
                target[1] = true;
            } else if (value == "3xx") {
                target[2] = true;
            } else if (value == "4xx") {
                target[3] = true;
            } else if (value == "5xx") {
                target[4] = true;
            }
        }
    }

    void capture_request_log(const http1_request_reader& parsed, const v2::Website& website) {
        request_body_bytes_ = request_body_bytes(parsed);
        if (website.access_log_query_params()) {
            query_string_ = query_string_for_log(parsed.request().target());
        }
        if (website.access_log_cookies()) {
            if (const auto cookies = parsed.request().header("Cookie")) {
                cookies_ = node_log_buffer::cookie_value(*cookies);
            }
        }
        if (website.access_log_referer()) {
            if (const auto referer = parsed.request().header("Referer")) {
                request_referer_.assign(*referer);
            }
        }
        if (website.access_log_user_agent()) {
            if (const auto user_agent = parsed.request().header("User-Agent")) {
                request_user_agent_.assign(*user_agent);
            }
        }
        if (website.access_log_request_headers()) {
            request_headers_ = request_header_block(parsed);
        }
        if (website.access_log_request_body()) {
            auto [body, truncated] = request_body_for_log(parsed);
            request_body_ = std::move(body);
            request_body_truncated_ = truncated;
        }
    }

    static std::uint64_t request_body_bytes(const http1_request_reader& parsed) {
        const auto body = parsed.body();
        return body ? static_cast<std::uint64_t>(body->size()) : 0;
    }

    void capture_response_headers(std::string_view bytes) {
        if (!access_log_response_headers_enabled_ || !response_headers_.empty()) {
            return;
        }
        response_headers_ =
            header_block_for_log(http1_response_headers(bytes), node_log_buffer::max_response_headers_bytes);
    }

    bool access_log_status_code_enabled(std::uint32_t status_code) const noexcept {
        if (status_code == 499) {
            return access_log_client_abort_enabled_;
        }
        const auto range = status_code / 100;
        return range >= 1 && range <= 5 && access_log_status_code_ranges_[range - 1];
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
        clients_.opened(key);
        client_key_ = key;
    }

    std::string client_ip() const noexcept {
        return client_address_.empty() ? std::string("unknown") : client_address_;
    }

    void record_access(std::uint32_t status_code, std::uint64_t response_bytes) noexcept {
        if (access_recorded_ || !access_log_enabled_ || website_id_.empty() || status_code == 0) {
            return;
        }
        if (!access_log_status_code_enabled(status_code)) {
            return;
        }
        access_recorded_ = true;
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_started_at_);
        const auto duration_ms =
            duration.count() < 0 ? std::uint64_t{} : static_cast<std::uint64_t>(duration.count());
        logs_.access({
            .website_id_ = website_id_,
            .client_ip_ = client_ip(),
            .protocol_ = secure_ ? "https" : "http",
            .method_ = request_method_,
            .host_ = request_host_,
            .target_ = request_target_,
            .status_code_ = status_code,
            .request_bytes_ = request_body_bytes_,
            .response_bytes_ = response_bytes,
            .duration_ms_ = duration_ms,
            .user_agent_ = request_user_agent_,
            .referer_ = request_referer_,
            .request_headers_ = request_headers_,
            .request_body_ = request_body_,
            .request_body_truncated_ = request_body_truncated_,
            .tls_fingerprint_ = tls_fingerprint_,
            .response_headers_ = response_headers_,
            .query_string_ = query_string_,
            .cookies_ = cookies_,
        });
    }

public:
    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        ++origin_attempt_;
        std::error_code ignored;
        timer_.cancel(ignored);
        discard_origin_transport();
        if (buffered_exchange_) {
            buffered_exchange_->close();
            buffered_exchange_.reset();
        }
        client_socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        ignored = client_socket().close(ignored);
    }

private:
    void send(std::string bytes, buffered_bytes_lease reservation = {}) {
        if (closed_ || response_pending_) {
            return;
        }
        response_pending_ = true;
        cancel_timer();
        discard_origin_transport();
        if (buffered_exchange_) {
            buffered_exchange_->close();
            buffered_exchange_.reset();
        }
        capture_response_headers(bytes);
        response_reservation_ = std::move(reservation);
        response_bytes_ = std::move(bytes);
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(response_bytes_),
            [self](const std::error_code& error, std::size_t size) {
                self->metrics_.traffic_out(size);
                self->record_access(error && self->access_log_client_abort_enabled_
                                        ? 499
                                        : wire_status_code(self->response_bytes_),
                    size);
                if (error) {
                    self->log_client_io("client response write failed", error);
                }
                self->close();
            });
    }

    void handle_request(const http1_request_reader& parsed) {
        const auto config = runtime_.config();
        if (!config) {
            send(response("503 Service Unavailable"));
            return;
        }
        const auto host = parsed.request().header("Host");
        if (!host) {
            send(response("400 Bad Request"));
            return;
        }
        request_method_.assign(parsed.request().method());
        request_target_.assign(parsed.request().target());
        request_host_.assign(*host);
        const v2::Website* website{};
        try {
            website = config->website(*host);
        } catch (const invalid_hostname& error) {
            log("request rejected: invalid HTTP Host header", error, "warning");
            send(response("400 Bad Request"));
            return;
        }
        if (website != nullptr) {
            website_id_ = website->id();
            access_log_enabled_ = website->access_log_enabled();
            if (access_log_enabled_) {
                access_log_response_headers_enabled_ = website->access_log_response_headers();
                access_log_client_abort_enabled_ = website->access_log_client_abort();
                apply_status_code_ranges(access_log_status_code_ranges_, *website);
                capture_request_log(parsed, *website);
            }
        }
        if (!secure_) {
            const auto redirect = http_to_https_redirect(*config, *host, parsed.request().target());
            if (redirect) {
                send(response("301 Moved Permanently", "Location: " + redirect->location_ + "\r\n"));
                return;
            }
        }
        if (website == nullptr) {
            send(response("421 Misdirected Request"));
            return;
        }
        hsts_enabled_ = secure_ && website->hsts_enabled();
        flexedge::route_policy::header_values match_headers;
        for (const auto& header : parsed.request().headers()) {
            match_headers.emplace_back(header.name(), header.value());
        }
        effective_route_ = evaluate_route_rules(*website, parsed.request().method(), parsed.request().target(),
            *host, match_headers, &config->route_patterns());
        if (!effective_route_) {
            send(response("400 Bad Request"));
            return;
        }
        const auto* route = &*effective_route_;
        if (route != nullptr && route->action() == "redirect") {
            const auto location = route->redirect_url();
            if (location.empty()) {
                send(response("400 Bad Request"));
                return;
            }
            std::string headers =
                "Location: " + location + "\r\n";
            for (const auto& header : route->response_headers()) {
                headers.append(header.name()).append(": ").append(header.value()).append("\r\n");
            }
            send(response(flexedge::route_policy::redirect_status_line(route->redirect_status()),
                headers));
            return;
        }
        upgrade_requested_ = parsed.request().header("Upgrade").has_value();
        const auto consumed_bytes = parsed.message_bytes();
        if (upgrade_requested_ && consumed_bytes < request_bytes_.size()) {
            client_upgrade_remainder_.assign(request_bytes_.data() + consumed_bytes,
                request_bytes_.size() - consumed_bytes);
        }
        active_config_ = config;
        origins_ = origin_candidates(*website, sequence_, route);
        std::stable_partition(origins_.begin(), origins_.end(), [&](const v2::Origin* origin) {
            return health_.healthy(website->id(), origin->id());
        });
        website_id_ = website->id();
        healthy_threshold_ = website->healthy_threshold();
        unhealthy_threshold_ = website->unhealthy_threshold();
        if (origins_.empty()) {
            send(response("502 Bad Gateway"));
            return;
        }
        proxy(parsed, *website, *host, route);
    }

    void arm(std::chrono::seconds timeout, std::uint64_t attempt,
        timeout_action action = timeout_action::close) {
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

    void discard_origin_transport() noexcept {
        if (origin_transport_) {
            origin_transport_->close();
        }
        origin_transport_.reset();
        origin_connection_key_.reset();
        reused_origin_transport_ = false;
    }

    void release_origin_transport() noexcept {
        if (origin_transport_ && origin_connection_key_ && response_codec_ &&
            response_codec_->origin_reusable()) {
            origin_connections_.release(std::move(*origin_connection_key_),
                std::move(origin_transport_));
        } else {
            discard_origin_transport();
            return;
        }
        origin_connection_key_.reset();
        reused_origin_transport_ = false;
    }

    bool retry_stale_origin_transport() {
        if (!reused_origin_transport_ || retried_reused_origin_transport_ || origin_index_ == 0) {
            return false;
        }
        retried_reused_origin_transport_ = true;
        discard_origin_transport();
        --origin_index_;
        try_next_origin();
        return true;
    }

    [[nodiscard]] bool origin_attempt_current(std::uint64_t attempt) const noexcept {
        return !closed_ && !response_pending_ && attempt == origin_attempt_;
    }

    bool prepare_origin_request(const http1_request_reader& parsed, const v2::Website& website,
        std::string_view incoming_host, bool force_identity_encoding) {
        if (client_address_ == "unknown") {
            return false;
        }
        auto prepared = flexedge::node::prepare_origin_request(
            parsed, website, incoming_host, client_address_, secure_, upgrade_requested_, false,
            force_identity_encoding, active_route_, &active_config_->route_patterns());
        if (!prepared) {
            return false;
        }
        outbound_request_ = std::move(prepared->bytes_);
        response_codec_ = std::make_unique<origin_response_codec>(
            std::move(prepared->response_exchange_), hsts_enabled_, active_route_);
        buffered_request_.method_ = parsed.request().method();
        buffered_request_.target_ = route_target(parsed.request().target(), active_route_, &active_config_->route_patterns());
        buffered_request_.authority_ =
            website.origin_host_header().empty() || website.origin_host_header() == "$host"
                ? normalize_hostname(incoming_host)
                : website.origin_host_header();
        buffered_request_.headers_.clear();
        for (const auto& header : parsed.request().headers()) {
            buffered_request_.headers_.emplace_back(header.name(), header.value());
        }
        if (parsed.has_body()) {
            const auto body = parsed.body();
            if (!body) {
                return false;
            }
            buffered_request_.body_.assign(*body);
            buffered_request_.has_body_ = true;
        }
        return true;
    }

    std::optional<prepared_origin_request> prepare_buffered_origin_request() const {
        if (active_website_ == nullptr) {
            return std::nullopt;
        }
        std::vector<ruvia::http_header_view> headers;
        headers.reserve(buffered_request_.headers_.size());
        for (const auto& [name, value] : buffered_request_.headers_) {
            headers.emplace_back(name, value);
        }
        return flexedge::node::prepare_origin_request({.method_ = buffered_request_.method_,
                                                          // The codec rewrites once; buffered_request_
                                                          // retains the rewritten path for compression policy.
                                                          .target_ = request_target_,
                                                          .headers_ = headers,
                                                          .body_ = buffered_request_.body_,
                                                          .has_body_ = buffered_request_.has_body_},
            *active_website_, request_host_, client_address_,
            secure_, false, false, false, active_route_, &active_config_->route_patterns());
    }

    void proxy(const http1_request_reader& parsed, const v2::Website& website,
        std::string_view incoming_host, const v2::RouteRule* route) {
        request_host_.assign(incoming_host);
        active_website_ = &website;
        active_route_ = route;
        const auto accepts_streaming_response = [&] {
            const auto accept = parsed.request().header("Accept");
            return accept && flexedge::node::accepts_event_stream(*accept);
        }();
        buffered_origin_enabled_ = website.response_compression_enabled() &&
                                   parsed.request().header("Accept-Encoding").has_value() &&
                                   !upgrade_requested_ && !accepts_streaming_response;
        if (!prepare_origin_request(parsed, website, incoming_host, accepts_streaming_response)) {
            send(response("502 Bad Gateway"));
            return;
        }
        connect_timeout_ = std::chrono::seconds(website.origin_connect_timeout_seconds());
        read_timeout_ = std::chrono::seconds(website.origin_read_timeout_seconds());
        try_next_origin();
    }

    void send_buffered_response(buffered_proxy_response buffered_response) {
        const auto status_text = std::to_string(buffered_response.status_);
        const auto status = ruvia::http_status_code::from_value(buffered_response.status_);
        const auto reason = ruvia::http_reason_phrase(status);
        std::size_t header_bytes = std::string_view("HTTP/1.1 ").size() + status_text.size() +
                                   std::string_view("\r\n").size();
        if (!reason.empty()) {
            header_bytes += 1 + reason.size();
        }
        for (const auto& [name, value] : buffered_response.headers_) {
            if (should_forward_buffered_response_header(name, hsts_enabled_)) {
                header_bytes += name.size() + 2 + value.size() + 2;
            }
        }
        const auto body_size = buffered_response.body_.size();
        const auto source_reservation_bytes = buffered_response.reservation_.bytes();
        const auto length_text = std::to_string(body_size);
        header_bytes += std::string_view("Content-Length: ").size() + length_text.size() +
                        std::string_view("\r\nConnection: close\r\n").size();
        if (hsts_enabled_) {
            header_bytes +=
                std::string_view("Strict-Transport-Security: max-age=31536000\r\n").size();
        }
        header_bytes += 2;
        if (!buffered_response.reservation_.try_grow(header_bytes + body_size)) {
            send(response("503 Service Unavailable"));
            return;
        }

        std::string wire;
        wire.reserve(header_bytes + body_size);
        wire.append("HTTP/1.1 ").append(status_text);
        if (!reason.empty()) {
            wire.push_back(' ');
            wire.append(reason);
        }
        wire.append("\r\n");
        for (const auto& [name, value] : buffered_response.headers_) {
            if (should_forward_buffered_response_header(name, hsts_enabled_)) {
                wire.append(name).append(": ").append(value).append("\r\n");
            }
        }
        wire.append("Content-Length: ").append(length_text).append("\r\nConnection: close\r\n");
        if (hsts_enabled_) {
            wire.append("Strict-Transport-Security: max-age=31536000\r\n");
        }
        wire.append("\r\n").append(buffered_response.body_);
        buffered_response.body_.clear();
        buffered_response.body_.shrink_to_fit();
        buffered_response.reservation_.shrink(source_reservation_bytes);
        send(std::move(wire), std::move(buffered_response.reservation_));
    }

    void complete_buffered_origin_attempt(std::uint64_t attempt, std::error_code error,
        buffered_proxy_response response) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        buffered_exchange_.reset();
        cancel_timer();
        if (error) {
            if (buffered_bytes_exhausted(error)) {
                send(this->response("503 Service Unavailable"));
                return;
            }
            log_io("buffered origin exchange failed", error);
            current_health_.failure(unhealthy_threshold_);
            try_next_origin();
            return;
        }
        current_health_.success(healthy_threshold_);
        const auto* website = active_config_->website(buffered_request_.authority_);
        if (website == nullptr) {
            website = active_config_->website(request_host_);
        }
        if (website != nullptr) {
            apply_response_compression(buffered_request_, *website, response);
        }
        if (active_route_ != nullptr) {
            apply_route_headers(response.headers_, active_route_->response_headers());
        }
        send_buffered_response(std::move(response));
    }

    void start_buffered_origin_attempt(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        auto prepared = prepare_buffered_origin_request();
        if (!prepared) {
            send(response("502 Bad Gateway"));
            return;
        }
        arm(connect_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        buffered_exchange_ = std::make_shared<buffered_origin_exchange>(
            stream_.get_executor(), origin_connections_, std::move(prepared->bytes_),
            std::move(prepared->response_exchange_), origin_host_, origin_port_, origin_secure_,
            connect_timeout_ + read_timeout_, response_buffers_,
            [self, attempt](std::error_code error, buffered_proxy_response response) {
                self->complete_buffered_origin_attempt(attempt, std::move(error), std::move(response));
            });
        buffered_exchange_->start();
    }

    void complete_origin_connection(std::uint64_t attempt, const std::error_code& error) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        if (error) {
            log_io("origin connect failed", error);
            current_health_.failure(unhealthy_threshold_);
            try_next_origin();
            return;
        }
        write_origin_request(attempt);
    }

    void start_direct_origin_attempt(std::uint64_t attempt) {
        origin_connection_key_ = {
            .host_ = origin_host_,
            .port_ = origin_port_,
            .secure_ = origin_secure_,
        };
        origin_transport_ = origin_connections_.acquire(*origin_connection_key_);
        reused_origin_transport_ = origin_transport_->connected();
        if (reused_origin_transport_) {
            write_origin_request(attempt);
            return;
        }
        arm(connect_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        origin_transport_->connect(origin_host_, origin_port_, origin_secure_,
            [self, attempt](const std::error_code& error) {
                self->complete_origin_connection(attempt, error);
            });
    }

    void reset_origin_attempt() {
        ++origin_attempt_;
        cancel_timer();
        if (buffered_exchange_) {
            buffered_exchange_->close();
            buffered_exchange_.reset();
        }
        discard_origin_transport();
    }

    void try_next_origin() {
        reset_origin_attempt();
        if (origin_index_ >= origins_.size()) {
            send(response("502 Bad Gateway"));
            return;
        }
        const auto* origin = origins_[origin_index_++];
        origin_host_ = origin->host();
        current_health_ = health_.get_target(website_id_, origin->id());
        origin_port_ = static_cast<std::uint16_t>(origin->port());
        origin_secure_ = origin->protocol() == "https";
        const auto attempt = origin_attempt_;
        if (buffered_origin_enabled_) {
            start_buffered_origin_attempt(attempt);
            return;
        }
        start_direct_origin_attempt(attempt);
    }

    void write_origin_request(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        arm(connect_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                if (size == 0 && self->retry_stale_origin_transport()) {
                    return;
                }
                self->log_io("origin request write failed", error);
                self->current_health_.failure(self->unhealthy_threshold_);
                self->try_next_origin();
                return;
            }
            self->read_origin_head(attempt);
        };
        origin_transport_->write(asio::buffer(outbound_request_), std::move(completion));
    }

    void read_origin_head(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        arm(read_timeout_, attempt, timeout_action::failover);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->log_io("origin response head read failed", error);
                self->current_health_.failure(self->unhealthy_threshold_);
                self->try_next_origin();
                return;
            }
            const auto status = self->response_codec_->consume_head(
                std::string_view(self->origin_read_buffer_.data(), size));
            if (status == origin_response_head_status::need_more) {
                self->read_origin_head(attempt);
                return;
            }
            if (status == origin_response_head_status::failure) {
                self->log("origin response head parse failed");
                self->current_health_.failure(self->unhealthy_threshold_);
                self->try_next_origin();
                return;
            }
            self->tunnel_ = self->response_codec_->tunnel();
            self->relay_bytes_ = self->response_codec_->take_output();
            self->capture_response_headers(self->relay_bytes_);
            self->current_health_.success(self->healthy_threshold_);
            self->relay_origin_bytes(attempt);
        };
        origin_transport_->read(asio::buffer(origin_read_buffer_), std::move(completion));
    }

    void relay_origin_bytes(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        arm(read_timeout_, attempt);
        const auto self = this->shared_from_this();
        asio::async_write(
            stream_, asio::buffer(relay_bytes_),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                self->metrics_.traffic_out(size);
                self->proxied_response_bytes_ += size;
                if (error) {
                    if (self->access_log_client_abort_enabled_) {
                        self->record_access(499, self->proxied_response_bytes_);
                    }
                    self->log_client_io("client relay write failed", error);
                    self->close();
                    return;
                }
                if (self->tunnel_) {
                    self->record_access(self->response_codec_->status_code().value_or(101),
                        self->proxied_response_bytes_);
                    self->start_tunnel(attempt);
                } else if (self->response_codec_->complete()) {
                    self->record_access(self->response_codec_->status_code().value_or(502),
                        self->proxied_response_bytes_);
                    self->release_origin_transport();
                    self->close();
                } else {
                    self->read_origin_body(attempt);
                }
            });
    }

    void start_tunnel(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        cancel_timer();
        if (client_upgrade_remainder_.empty()) {
            read_tunnel_client(attempt);
            read_tunnel_origin(attempt);
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t) {
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
        };
        origin_transport_->write(asio::buffer(client_upgrade_remainder_), std::move(completion));
    }

    void read_tunnel_client(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
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
                self->write_tunnel_origin(attempt, size);
            });
    }

    void write_tunnel_origin(std::uint64_t attempt, std::size_t size) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            self->read_tunnel_client(attempt);
        };
        origin_transport_->write(asio::buffer(client_tunnel_buffer_.data(), size),
            std::move(completion));
    }

    void read_tunnel_origin(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error) {
                self->close();
                return;
            }
            self->write_tunnel_client(attempt, size);
        };
        origin_transport_->read(asio::buffer(origin_tunnel_buffer_), std::move(completion));
    }

    void write_tunnel_client(std::uint64_t attempt, std::size_t size) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(origin_tunnel_buffer_.data(), size),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                self->metrics_.traffic_out(size);
                if (error) {
                    self->close();
                    return;
                }
                self->read_tunnel_origin(attempt);
            });
    }

    void read_origin_body(std::uint64_t attempt) {
        if (!origin_attempt_current(attempt)) {
            return;
        }
        arm(read_timeout_, attempt);
        const auto self = this->shared_from_this();
        auto completion = [self, attempt](const std::error_code& error, std::size_t size) {
            if (!self->origin_attempt_current(attempt)) {
                return;
            }
            if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
                if (self->response_codec_) {
                    self->record_access(self->response_codec_->status_code().value_or(502),
                        self->proxied_response_bytes_);
                }
                self->close();
                return;
            }
            if (error) {
                self->log_io("origin response body read failed", error);
                self->close();
                return;
            }
            if (!self->response_codec_->consume_body(
                    std::string_view(self->origin_read_buffer_.data(), size))) {
                self->log("origin response body parse failed");
                self->close();
                return;
            }
            self->relay_bytes_.assign(self->origin_read_buffer_.data(), size);
            self->relay_origin_bytes(attempt);
        };
        origin_transport_->read(asio::buffer(origin_read_buffer_), std::move(completion));
    }

    void consume() noexcept {
        try {
            consume_checked();
        } catch (const std::exception& error) {
            log("request handling failed", error);
            close();
        } catch (...) {
            log("request handling failed with unknown exception");
            close();
        }
    }

    void consume_checked() {
        switch (request_reader_.consume(request_bytes_)) {
            case http1_request_read_status::complete:
                handle_request(request_reader_);
                return;
            case http1_request_read_status::bad_request:
                send(response("400 Bad Request"));
                return;
            case http1_request_read_status::too_large:
                send(response("413 Content Too Large"));
                return;
            case http1_request_read_status::need_more:
                read();
                return;
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
                send(response("503 Service Unavailable"));
                return;
            }
            request_bytes_.append(read_buffer_.data(), size);
            consume();
        } catch (const std::exception& error) {
            log("client input handling failed", error);
            close();
        } catch (...) {
            log("client input handling failed with unknown exception");
            close();
        }
    }

    [[nodiscard]] decltype(auto) client_socket() noexcept {
        return stream_.lowest_layer();
    }

    client_stream_type stream_;
    origin_connection_pool& origin_connections_;
    asio::steady_timer timer_;
    runtime_state& runtime_;
    origin_health_registry& health_;
    runtime_metrics& metrics_;
    connected_clients& clients_;
    node_log_buffer::producer& logs_;
    buffered_bytes_budget& response_buffers_;
    http1_request_reader request_reader_{max_request_bytes};
    std::array<char, 8192> read_buffer_{};
    std::string request_bytes_;
    std::string response_bytes_;
    buffered_bytes_lease request_reservation_;
    buffered_bytes_lease response_reservation_;
    std::string origin_host_;
    std::string website_id_;
    std::string request_host_;
    origin_health_registry::target current_health_;
    std::string client_address_;
    std::optional<connected_clients::address_key> client_key_;
    std::string request_method_;
    std::string request_target_;
    std::uint64_t request_body_bytes_{};
    std::string request_user_agent_;
    std::string request_referer_;
    std::string request_headers_;
    std::string request_body_;
    std::string response_headers_;
    std::string query_string_;
    std::string cookies_;
    std::string tls_fingerprint_;
    std::uint16_t origin_port_{};
    std::chrono::seconds connect_timeout_{10};
    std::chrono::seconds read_timeout_{30};
    std::string outbound_request_;
    std::unique_ptr<origin_transport> origin_transport_;
    std::optional<origin_connection_key> origin_connection_key_;
    std::unique_ptr<origin_response_codec> response_codec_;
    std::shared_ptr<buffered_origin_exchange> buffered_exchange_;
    buffered_proxy_request buffered_request_;
    std::array<char, 8192> origin_read_buffer_{};
    std::array<char, 16384> client_tunnel_buffer_{};
    std::array<char, 16384> origin_tunnel_buffer_{};
    std::string relay_bytes_;
    std::string client_upgrade_remainder_;
    std::shared_ptr<const compiled_config> active_config_;
    const v2::Website* active_website_{};
    std::optional<v2::RouteRule> effective_route_;
    const v2::RouteRule* active_route_{};
    std::vector<const v2::Origin*> origins_;
    std::size_t origin_index_{};
    std::uint64_t origin_attempt_{};
    std::uint64_t proxied_response_bytes_{};
    std::uint32_t healthy_threshold_{1};
    std::uint32_t unhealthy_threshold_{1};
    std::uint64_t sequence_{};
    std::shared_ptr<const void> transport_state_;
    bool secure_{};
    std::function<void()> on_retired_;
    bool origin_secure_{};
    bool buffered_origin_enabled_{};
    bool reused_origin_transport_{};
    bool retried_reused_origin_transport_{};
    bool upgrade_requested_{};
    bool tunnel_{};
    bool hsts_enabled_{};
    bool closed_{};
    bool response_pending_{};
    bool access_recorded_{};
    bool access_log_enabled_{};
    bool access_log_response_headers_enabled_{};
    bool access_log_client_abort_enabled_{};
    bool request_body_truncated_{};
    std::array<bool, 5> access_log_status_code_ranges_{true, true, true, true, true};
    std::chrono::steady_clock::time_point request_started_at_{std::chrono::steady_clock::now()};
};

class http_listener final : public std::enable_shared_from_this<http_listener> {
public:
    template <typename... args_type>
    [[nodiscard]] static std::shared_ptr<http_listener> create(args_type&&... args) {
        auto listener = std::shared_ptr<http_listener>(new http_listener(std::forward<args_type>(args)...));
        listener->stop_registration_ = listener->owner_.on_stop(
            [weak = std::weak_ptr<http_listener>(listener)]() -> ruvia::task<void> {
                if (const auto self = weak.lock()) {
                    co_await self->stop_and_wait();
                }
                co_return;
            });
        return listener;
    }

private:
    http_listener(ruvia::event_loop owner, runtime_state& runtime, origin_health_registry& health,
        runtime_metrics& metrics, connected_clients& clients, node_log_buffer::producer& logs,
        origin_connection_pool& origin_connections, buffered_bytes_budget& request_buffers,
        buffered_bytes_budget& response_buffers, const asio::ip::tcp::endpoint& endpoint)
        : owner_(std::move(owner)),
          owner_worker_(owner_.handle()),
          runtime_(runtime),
          health_(health),
          metrics_(metrics),
          clients_(clients),
          logs_(logs),
          origin_connections_(origin_connections),
          request_buffers_(request_buffers),
          response_buffers_(response_buffers),
          acceptor_(owner_.io_context()),
          activation_timer_(owner_.io_context()),
          stop_signal_(owner_worker_) {
        std::error_code error;
        error = acceptor_.open(endpoint.protocol(), error);
        if (!error) {
            error = acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        }
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        if (!error) {
            int enabled = 1;
            if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
                    sizeof(enabled)) != 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not enable SO_REUSEPORT");
            }
        }
#endif
        if (!error) {
            error = acceptor_.bind(endpoint, error);
        }
        if (!error) {
            error = acceptor_.listen(asio::socket_base::max_listen_connections, error);
        }
        if (error) {
            throw std::system_error(error, "could not start edge HTTP listener");
        }
    }

public:
    ~http_listener() {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_ || start_requested_) {
            return;
        }
        stopping_ = true;
        std::error_code ignored;
        ignored = acceptor_.close(ignored);
    }

private:
    void start() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        if (activation_gate_ && !activation_gate_->active()) {
            const auto self = shared_from_this();
            bool operation_started = false;
            try {
                activation_timer_.expires_after(std::chrono::milliseconds(1));
                ++pending_operations_;
                operation_started = true;
                activation_timer_.async_wait([self](const std::error_code& error) {
                    self->operation_completed();
                    if (!error) {
                        self->start();
                    }
                });
            } catch (...) {
                if (operation_started) {
                    operation_completed();
                }
                stop();
            }
            return;
        }
        accept();
    }

public:
    void request_start(std::shared_ptr<const listener_activation_gate> activation_gate = nullptr) {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_) {
            throw std::runtime_error("edge HTTP listener worker is stopping");
        }
        start_requested_ = true;
        const auto self = shared_from_this();
        try {
            if (!owner_.post([self, gate = std::move(activation_gate)] {
                           if (self->started_) {
                               return;
                           }
                           self->started_ = true;
                           self->activation_gate_ = gate;
                           self->start();
                       })
                    .accepted()) {
                throw std::runtime_error("edge HTTP listener worker is stopping");
            }
        } catch (...) {
            start_requested_ = false;
            throw;
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint() const {
        return acceptor_.local_endpoint();
    }

private:
    // Socket cancellation and accept initiation must stay on the owning loop.
    ruvia::task<void> stop_and_wait() {
        {
            std::lock_guard lock(lifecycle_mutex_);
            stop();
        }
        while (pending_operations_ != 0 || active_sessions_ != 0) {
            co_await stop_signal_.wait();
        }
        std::lock_guard lock(lifecycle_mutex_);
        loop_stop_complete_ = true;
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        activation_timer_.cancel(ignored);
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
        for (const auto& weak : sessions_) {
            if (const auto session = weak.lock()) {
                session->close();
            }
        }
    }

public:
    void request_stop() noexcept {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_) {
            return;
        }
        if (!start_requested_) {
            stopping_ = true;
            std::error_code ignored;
            ignored = acceptor_.close(ignored);
            loop_stop_complete_ = true;
            return;
        }
        if (owner_.is_current()) {
            stop();
            return;
        }
        asio::post(owner_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->stop();
            }
        });
    }

private:
    // Accept completions and session retirement run on the owning loop.
    void operation_completed() noexcept {
        --pending_operations_;
        stop_signal_.notify();
    }

    void session_retired() noexcept {
        --active_sessions_;
        stop_signal_.notify();
    }

    void accept() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::shared_ptr<http_listener> self;
        try {
            socket = std::make_shared<asio::ip::tcp::socket>(owner_.io_context());
            self = shared_from_this();
        } catch (...) {
            stop();
            return;
        }
        ++pending_operations_;
        try {
            acceptor_.async_accept(
                *socket,
                asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                    self->operation_completed();
                    if (!error && !self->stopping_) {
                        try {
                            auto session = std::make_shared<basic_http_session<asio::ip::tcp::socket>>(
                                std::move(*socket), self->runtime_, self->health_, self->metrics_,
                                self->clients_, self->logs_, self->origin_connections_,
                                self->request_buffers_, self->response_buffers_, ++self->sequence_,
                                nullptr, false, std::string{}, [weak = self->weak_from_this()] {
                                    if (const auto listener = weak.lock()) {
                                        listener->session_retired();
                                    }
                                });
                            ++self->active_sessions_;
                            std::erase_if(self->sessions_, [](const auto& active) {
                                return active.expired();
                            });
                            self->sessions_.push_back(session);
                            try {
                                session->start();
                            } catch (...) {
                                session->close();
                            }
                        } catch (...) {
                        }
                    }
                    if (self->acceptor_.is_open()) {
                        self->accept();
                    }
                }));
        } catch (...) {
            operation_completed();
            stop();
        }
    }

    ruvia::event_loop owner_;
    // Borrowed by stop_signal_; declared first so it outlives every signal wait.
    ruvia::worker_handle owner_worker_;
    std::mutex lifecycle_mutex_;
    bool start_requested_{};
    bool loop_stop_complete_{};
    runtime_state& runtime_;
    origin_health_registry& health_;
    runtime_metrics& metrics_;
    connected_clients& clients_;
    node_log_buffer::producer& logs_;
    origin_connection_pool& origin_connections_;
    buffered_bytes_budget& request_buffers_;
    buffered_bytes_budget& response_buffers_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer activation_timer_;
    ruvia::event_loop_stop_registration stop_registration_;
    ruvia::worker_signal stop_signal_;
    std::size_t pending_operations_{};
    std::size_t active_sessions_{};
    std::vector<std::weak_ptr<basic_http_session<asio::ip::tcp::socket>>> sessions_;
    bool stopping_{};
    std::shared_ptr<const listener_activation_gate> activation_gate_;
    std::uint64_t sequence_{};
    bool started_{};  // Accessed only by the owning event loop.
};

}  // namespace flexedge::node
