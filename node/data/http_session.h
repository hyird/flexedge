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
#include "node/data/response_cache.h"
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
    response_cache* cache_{};
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
    void retain_tls_context(std::shared_ptr<tls_context_set> contexts, std::string server_name) noexcept {
        tls_contexts_ = std::move(contexts);
        tls_server_name_ = std::move(server_name);
    }

    [[nodiscard]] std::string_view tls_server_name() const noexcept {
        return tls_server_name_;
    }

private:
    std::function<void()> close_;
    std::function<void()> on_retired_;
    std::shared_ptr<tls_context_set> tls_contexts_;
    std::string tls_server_name_;
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
        std::string tls_fingerprint = {}, bool cache_prewarm = false, bool count_connection = true)
        : connection_(std::move(connection)),
          stream_(std::move(stream)),
          context_(context),
          timer_(stream_.get_executor()),
          access_log_(context_.logs_, secure, std::move(tls_fingerprint)),
          request_reservation_(context_.request_buffers_.lease()),
          sequence_(sequence),
          secure_(secure),
          cache_prewarm_(cache_prewarm),
          count_connection_(count_connection) {
        if (!cache_prewarm_) {
            capture_client_address(count_connection_);
            if (count_connection_) {
                context_.metrics_.connection_opened();
            }
        } else {
            client_address_ = "127.0.0.1";
        }
    }

    basic_http_session(const basic_http_session&) = delete;
    basic_http_session& operator=(const basic_http_session&) = delete;

    ~basic_http_session() {
        if (client_key_) {
            context_.clients_.closed(*client_key_);
        }
        if (!cache_prewarm_ && count_connection_) {
            context_.metrics_.connection_closed();
        }
    }

    void prewarm(std::string request, std::function<void(bool, std::string)> completion) {
        prewarm_completion_ = std::move(completion);
        if (!request_reservation_.try_grow(request.size())) {
            close();
            return;
        }
        request_bytes_ = std::move(request);
        consume();
    }

    void start(std::string initial = {}) {
        connection_->set_close([weak = this->weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->close();
            }
        });
        if (initial.empty()) {
            read();
        } else if (request_reservation_.try_grow(initial.size())) {
            request_bytes_ = std::move(initial);
            consume();
        } else {
            reply(ruvia::http_status::service_unavailable);
        }
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        if (cache_ticket_ && context_.cache_) {
            try {
                context_.cache_->abandon(*cache_ticket_);
            } catch (...) {
            }
            cache_ticket_.reset();
        }
        if (prewarm_completion_) {
            auto completion = std::move(prewarm_completion_);
            try {
                completion(false, "预热请求未完成");
            } catch (...) {
            }
        }
        ++origin_attempt_;
        cancel_timer();
        discard_exchange();
        std::error_code ignored;
        client_socket().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        ignored = client_socket().close(ignored);
    }

private:
    static constexpr std::size_t max_request_bytes = 2 * 1024 * 1024;
    using cached_file_buffer = std::array<char, 65536>;

    enum class timeout_action { close,
        failover };

    struct tunnel_state final {
        std::string client_upgrade_remainder_;
        std::array<char, 16384> client_tunnel_buffer_{};
    };

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
    void capture_client_address(bool account = true) {
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
        if (account) {
            context_.clients_.opened(key);
            client_key_ = key;
        }
    }

    void send(std::string bytes, std::uint32_t status_code, buffered_bytes_lease reservation = {}) {
        if (closed_ || response_pending_) {
            return;
        }
        if (cache_prewarm_) {
            auto completion = std::move(prewarm_completion_);
            if (completion) {
                completion(cache_stored_, cache_stored_ ? "" : "响应未满足缓存条件，状态码 " + std::to_string(status_code));
            }
            close();
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

    std::optional<std::pair<std::string_view, std::string>>
    resolve_request_host(const http1_request_reader& parsed) {
        if (std::ranges::count_if(parsed.request().headers(), [](const auto& header) {
                return flexedge::route_policy::ascii_equal(header.name(), "Host");
            }) != 1) {
            reply(ruvia::http_status::bad_request);
            return std::nullopt;
        }
        const auto host = parsed.request().header("Host");
        if (!host) {
            reply(ruvia::http_status::bad_request);
            return std::nullopt;
        }
        access_log_.request(parsed, *host);
        auto hostname = flexedge::route_policy::hostname(*host);
        if (hostname.empty() || hostname.size() > 253) {
            log(hostname.empty() ? "request rejected: invalid HTTP Host header: empty hostname"
                                 : "request rejected: invalid HTTP Host header: hostname exceeds 253 bytes");
            reply(ruvia::http_status::bad_request);
            return std::nullopt;
        }
        return std::pair{*host, std::move(hostname)};
    }

    bool resolve_route_policy(const http1_request_reader& parsed, const v2::Website& website,
        std::string_view host) {
        flexedge::route_policy::header_values match_headers;
        match_headers.reserve(parsed.request().headers().size());
        for (const auto& header : parsed.request().headers()) {
            match_headers.emplace_back(header.name(), header.value());
        }
        effective_route_ = evaluate_route_rules(active_execution_->routes(), website,
            parsed.request().method(), parsed.request().target(), host, match_headers);
        if (!effective_route_) {
            reply(ruvia::http_status::bad_request);
            return false;
        }
        route_ = &*effective_route_;
        return true;
    }

    bool select_origin_candidates(const v2::Website& website) {
        origins_ = origin_cursor(route_->origins(), sequence_, context_.health_.snapshot());
        healthy_threshold_ = website.healthy_threshold();
        unhealthy_threshold_ = website.unhealthy_threshold();
        return !route_->origins().empty();
    }

    void handle_request(const http1_request_reader& parsed) {
        active_execution_ = context_.runtime_.execution();
        if (!active_execution_) {
            reply(ruvia::http_status::service_unavailable);
            return;
        }
        const auto resolved_host = resolve_request_host(parsed);
        if (!resolved_host) {
            return;
        }
        const auto& [host, hostname] = *resolved_host;
        website_ = active_execution_->config().website(hostname);
        if (website_ != nullptr) {
            access_log_.website(parsed, *website_);
        }
        if (!secure_) {
            if (const auto redirect_target =
                    http_to_https_redirect(active_execution_->config(), hostname, parsed.request().target())) {
                reply(redirect_target->status_, "Location: " + redirect_target->location_ + "\r\n");
                return;
            }
        }
        if (website_ == nullptr) {
            reply(ruvia::http_status::misdirected_request);
            return;
        }
        if (secure_ && !cache_prewarm_) {
            const auto server_name = connection_->tls_server_name();
            const auto* domain = active_execution_->config().domain(hostname);
            if (server_name.empty() || hostname != flexedge::route_policy::hostname(server_name) ||
                domain == nullptr || !domain->https_enabled() || !website_->https_enabled()) {
                reply(ruvia::http_status::misdirected_request);
                return;
            }
        }
        hsts_enabled_ = secure_ && website_->hsts_enabled();
        if (!resolve_route_policy(parsed, *website_, host)) {
            return;
        }
        if (route_->action() == "redirect") {
            redirect(*route_);
            return;
        }
        upgrade_requested_ = parsed.request().header("Upgrade").has_value();
        if (try_cache(parsed, host)) {
            return;
        }
        if (!select_origin_candidates(*website_)) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        proxy(parsed, host);
    }

    void redirect(const evaluated_route_policy& route) {
        const auto& location = route.redirect_url();
        if (location.empty()) {
            reply(ruvia::http_status::bad_request);
            return;
        }
        std::string headers{"Location: "};
        headers.append(location).append("\r\n");
        for (const auto& header : route.response_headers()) {
            headers.append(header.name()).append(": ").append(header.value()).append("\r\n");
        }
        const auto status = flexedge::route_policy::redirect_status(route.redirect_status())
                                ? static_cast<std::uint16_t>(route.redirect_status())
                                : std::uint16_t{302};
        reply(ruvia::http_status_code::from_value(status), headers);
    }

    void proxy(const http1_request_reader& parsed, std::string_view host) {
        if (client_address_ == "unknown") {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        const auto accept = parsed.request().header("Accept");
        force_identity_encoding_ = accept && accepts_event_stream(*accept);
        const auto accept_encoding = parsed.request().header("Accept-Encoding");
        buffered_ = cache_prewarm_ || (website_->response_compression_enabled() && accept_encoding.has_value() &&
                                          !upgrade_requested_ && !force_identity_encoding_);
        if (buffered_) {
            accept_encoding_ = accept_encoding.value_or(std::string_view{});
        }
        auto prepared = prepare_origin_request(parsed, *website_, host, client_address_,
            secure_, upgrade_requested_, false, force_identity_encoding_ || cache_ticket_.has_value(), route_);
        if (!prepared) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        prepared_request_.emplace(std::move(*prepared));
        connect_timeout_ = std::chrono::seconds(website_->origin_connect_timeout_seconds());
        read_timeout_ = std::chrono::seconds(website_->origin_read_timeout_seconds());
        try_next_origin();
    }

    bool try_cache(const http1_request_reader& parsed, std::string_view host) {
        if (!context_.cache_ || !website_->has_response_cache() || parsed.request().target().size() > 4096) {
            return false;
        }
        request_headers_.clear();
        for (const auto& header : parsed.request().headers()) {
            request_headers_.emplace_back(header.name(), header.value());
        }
        if (!context_.cache_->eligible(parsed.request().method(), request_headers_)) {
            return false;
        }
        const auto* rule = context_.cache_->matching(*website_, parsed.request().target(), parsed.request().method());
        if (!rule || rule->action_ != "cache") {
            return false;
        }
        cache_rule_ = *rule;
        cache_url_ = flexedge::cache::parse_url(std::string(secure_ ? "https://" : "http://") + std::string(host) + std::string(parsed.request().target()));
        if (!cache_url_) {
            return false;
        }
        access_log_.cache_status("miss");
        auto lookup = context_.cache_->lookup(*website_, *cache_rule_, cache_url_->value_, request_headers_,
            parsed.request().method() == "GET", context_.response_buffers_.lease());
        if (lookup.response_) {
            access_log_.cache_status(lookup.tier_);
            send_cached(std::move(lookup.response_), parsed);
            return true;
        }
        if (lookup.ticket_) {
            cache_ticket_ = std::move(lookup.ticket_);
        }
        if (lookup.filling_ && cache_waits_ < 5) {
            ++cache_waits_;
            timer_.expires_after(std::chrono::milliseconds(20 * cache_waits_));
            timer_.async_wait([self = this->shared_from_this()](const std::error_code& error) {
                if (error || self->closed_) {
                    return;
                }
                if (!self->try_cache(self->request_reader_, *self->request_reader_.request().header("Host"))) {
                    if (!self->select_origin_candidates(*self->website_)) {
                        self->reply(ruvia::http_status::bad_gateway);
                        return;
                    }
                    self->proxy(self->request_reader_, *self->request_reader_.request().header("Host"));
                }
            });
            return true;
        }
        return false;
    }

    void commit_cache(buffered_proxy_response& response) {
        if (!cache_ticket_ || !cache_rule_ || !context_.cache_ || !cache_url_) {
            return;
        }
        const auto config = context_.runtime_.config();
        const auto* current = config ? config->website(cache_url_->hostname_) : nullptr;
        if (current && current->id() == website_->id() && current->revision() == website_->revision() &&
            current->has_response_cache() && current->response_cache().revision() == website_->response_cache().revision()) {
            cache_stored_ = context_.cache_->store(*cache_ticket_, website_->response_cache(), *cache_rule_, response, request_headers_);
        } else {
            context_.cache_->abandon(*cache_ticket_);
        }
        cache_ticket_.reset();
    }
    void commit_cache_capture() {
        if (exchange_) {
            if (auto captured = exchange_->response().take_capture()) {
                commit_cache(*captured);
            }
        }
    }
    static bool matches_cache_etag(std::string_view incoming, std::string_view etag) {
        if (incoming == "*") {
            return true;
        }
        if (etag.starts_with("W/")) {
            etag.remove_prefix(2);
        }
        while (!incoming.empty()) {
            const auto comma = incoming.find(',');
            auto candidate = flexedge::route_policy::trim_ows(incoming.substr(0, comma));
            if (candidate.starts_with("W/")) {
                candidate.remove_prefix(2);
            }
            if (!etag.empty() && candidate == etag) {
                return true;
            }
            if (comma == std::string_view::npos) {
                break;
            }
            incoming.remove_prefix(comma + 1);
        }
        return false;
    }
    void send_cached(std::shared_ptr<const cached_response> response, const http1_request_reader& parsed) {
        if (cache_prewarm_) {
            cache_stored_ = true;
            send({}, 200);
            return;
        }
        cancel_timer();
        cached_response_ = std::move(response);
        const auto etag = cache_header(cached_response_->headers_, "ETag");
        bool unchanged{};
        if (const auto condition = parsed.request().header("If-None-Match")) {
            unchanged = matches_cache_etag(*condition, etag);
        } else if (const auto modified_since = parsed.request().header("If-Modified-Since")) {
            const auto modified = cache_http_time(cache_header(cached_response_->headers_, "Last-Modified"));
            const auto requested = cache_http_time(*modified_since);
            unchanged = modified && requested && *modified <= *requested;
        }
        cached_status_ = unchanged ? 304 : cached_response_->status_;
        response_bytes_ = "HTTP/1.1 " + std::to_string(cached_status_) + " " +
                          std::string(ruvia::http_reason_phrase(ruvia::http_status_code::from_value(static_cast<std::uint16_t>(cached_status_)))) + "\r\n";
        bool has_date{};
        for (const auto& [name, value] : cached_response_->headers_) {
            if (flexedge::route_policy::ascii_equal(name, "Age") || flexedge::route_policy::ascii_equal(name, "X-Cache") ||
                flexedge::route_policy::ascii_equal(name, "Connection") || flexedge::route_policy::ascii_equal(name, "Content-Length") ||
                (hsts_enabled_ && flexedge::route_policy::ascii_equal(name, "Strict-Transport-Security")) ||
                (cached_response_->browser_ttl_seconds_ > 0 && (flexedge::route_policy::ascii_equal(name, "Cache-Control") ||
                                                                   flexedge::route_policy::ascii_equal(name, "Expires"))) ||
                (route_ && std::ranges::any_of(route_->response_headers(), [&](const auto& mutation) { return flexedge::route_policy::ascii_equal(name, mutation.name()); }))) {
                continue;
            }
            if (flexedge::route_policy::ascii_equal(name, "Date")) {
                has_date = true;
            }
            response_bytes_.append(name).append(": ").append(value).append("\r\n");
        }
        if (route_) {
            for (const auto& mutation : route_->response_headers()) {
                response_bytes_.append(mutation.name()).append(": ").append(mutation.value()).append("\r\n");
            }
        }
        if (!has_date) {
            response_bytes_.append("Date: ").append(cache_http_date(std::time(nullptr))).append("\r\n");
        }
        const auto age = cached_response_->initial_age_ + std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - cached_response_->stored_at_).count();
        response_bytes_.append("Age: ").append(std::to_string(age)).append("\r\nX-Cache: HIT\r\nConnection: close\r\n");
        if (cached_response_->browser_ttl_seconds_ > 0) {
            response_bytes_.append("Cache-Control: public, max-age=").append(std::to_string(cached_response_->browser_ttl_seconds_)).append("\r\n");
        }
        if (hsts_enabled_) {
            response_bytes_.append("Strict-Transport-Security: max-age=31536000\r\n");
        }
        if (!unchanged) {
            response_bytes_.append("Content-Length: ").append(std::to_string(cached_response_->body_bytes_)).append("\r\n");
        }
        response_bytes_.append("\r\n");
        access_log_.response_head(response_bytes_);
        cached_head_only_ = unchanged || parsed.request().method() == "HEAD";
        if (cached_response_->file_ && !cached_head_only_) {
            cached_file_.open(cached_response_->file_->path_, std::ios::binary);
            if (!cached_file_) {
                cached_response_.reset();
                reply(ruvia::http_status::bad_gateway);
                return;
            }
        }
        if (!cached_head_only_ && !cached_response_->body_ && cached_response_->body_bytes_ != 0) {
            // Allocate before committing headers; retain through every write callback.
            cached_buffer_ = std::make_unique_for_overwrite<cached_file_buffer>();
        }
        response_pending_ = true;
        const auto self = this->shared_from_this();
        asio::async_write(stream_, asio::buffer(response_bytes_), [self](const std::error_code& error, std::size_t bytes) {
            self->context_.metrics_.traffic_out(bytes);
            self->proxied_response_bytes_ += bytes;
            if (error || self->cached_head_only_) {
                self->finish_cached(error);
                return;
            }
            self->write_cached_body();
        });
    }
    void finish_cached(const std::error_code& error) {
        access_log_.record(error ? 499 : cached_status_, proxied_response_bytes_, client_address_);
        close();
    }
    void write_cached_body() {
        if (closed_) {
            return;
        }
        if (cached_offset_ == cached_response_->body_bytes_) {
            finish_cached({});
            return;
        }
        const auto remaining = cached_response_->body_bytes_ - cached_offset_;
        asio::const_buffer bytes;
        if (cached_response_->body_) {
            bytes = asio::buffer(cached_response_->body_->data() + cached_offset_, remaining);
        } else {
            const auto size = (std::min)(remaining, cached_buffer_->size());
            cached_file_.read(cached_buffer_->data(), static_cast<std::streamsize>(size));
            if (static_cast<std::size_t>(cached_file_.gcount()) != size) {
                finish_cached(std::make_error_code(std::errc::io_error));
                return;
            }
            bytes = asio::buffer(cached_buffer_->data(), size);
        }
        const auto self = this->shared_from_this();
        asio::async_write(stream_, bytes, [self](const std::error_code& error, std::size_t size) {
            self->context_.metrics_.traffic_out(size);
            self->proxied_response_bytes_ += size;
            self->cached_offset_ += size;
            if (error) {
                self->finish_cached(error);
                return;
            }
            self->write_cached_body();
        });
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
        const auto* origin = origins_.next();
        if (origin == nullptr) {
            reply(ruvia::http_status::bad_gateway);
            return;
        }
        current_health_ = context_.health_.target(website_->id(), origin->id());
        start_origin_attempt(*origin, origin_attempt_);
    }

    void start_origin_attempt(const v2::Origin& origin, std::uint64_t attempt) {
        auto response =
            buffered_ ? origin_response_codec(prepared_request_->make_exchange_state(), hsts_enabled_,
                            route_, context_.response_buffers_.lease())
                      : origin_response_codec(prepared_request_->make_exchange_state(), hsts_enabled_,
                            route_);
        if (cache_ticket_ && cache_rule_) {
            response.set_browser_cache_ttl(cache_rule_->browser_ttl_seconds_, cache_rule_->status_codes_);
        }
        if (website_->has_response_cache()) {
            response.set_cache_status(cache_ticket_ ? "MISS" : "BYPASS");
        }
        if (cache_ticket_ && !buffered_) {
            response.capture_for_cache(static_cast<std::size_t>(website_->response_cache().max_object_bytes()), context_.response_buffers_.lease());
        }
        exchange_ = std::make_shared<origin_exchange>(context_.origin_connections_,
            origin_connection_key{
                .host_ = origin.host(),
                .port_ = static_cast<std::uint16_t>(origin.port()),
                .secure_ = origin.protocol() == "https",
                .allow_http2_ = !upgrade_requested_ && website_->origin_http_version() != "http1",
                .require_http2_ = !upgrade_requested_ && website_->origin_http_version() == "http2",
            },
            std::string_view(prepared_request_->bytes_), std::move(response));
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
        if (cache_prewarm_ && website_->has_response_cache() && exchange_->response().buffered_body_size() > static_cast<std::size_t>(website_->response_cache().max_object_bytes())) {
            reply(ruvia::http_status::content_too_large);
            return;
        }
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
        apply_response_compression(*website_, prepared_request_->target_, accept_encoding_, response);
        commit_cache(response);
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
                    self->commit_cache_capture();
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
                if (response.eof_completes_response()) {
                    self->commit_cache_capture();
                }
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
        tunnel_state_ = std::make_unique<tunnel_state>();
        if (upgrade_requested_ && !request_reader_.unconsumed_bytes().empty()) {
            tunnel_state_->client_upgrade_remainder_.assign(request_reader_.unconsumed_bytes());
        }
        exchange_->enter_tunnel();
        cancel_timer();
        if (tunnel_state_->client_upgrade_remainder_.empty()) {
            read_tunnel_client(attempt);
            read_tunnel_origin(attempt);
            return;
        }
        const auto self = this->shared_from_this();
        exchange_->write(asio::buffer(tunnel_state_->client_upgrade_remainder_),
            [self, attempt](const std::error_code& error) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                if (error) {
                    self->close();
                    return;
                }
                self->tunnel_state_->client_upgrade_remainder_.clear();
                self->read_tunnel_client(attempt);
                self->read_tunnel_origin(attempt);
            });
    }

    void read_tunnel_client(std::uint64_t attempt) {
        const auto self = this->shared_from_this();
        stream_.async_read_some(asio::buffer(tunnel_state_->client_tunnel_buffer_),
            [self, attempt](const std::error_code& error, std::size_t size) {
                if (!self->origin_attempt_current(attempt)) {
                    return;
                }
                if (error) {
                    self->close();
                    return;
                }
                self->exchange_->write(
                    asio::buffer(self->tunnel_state_->client_tunnel_buffer_.data(), size),
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
                    if (!continue_sent_ && request_reader_.expects_continue()) {
                        continue_sent_ = true;
                        send_continue();
                    } else {
                        read();
                    }
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

    void send_continue() {
        static constexpr std::string_view bytes = "HTTP/1.1 100 Continue\r\n\r\n";
        const auto self = this->shared_from_this();
        // No read or final write overlaps this interim response.
        asio::async_write(stream_, asio::buffer(bytes),
            [self](const std::error_code& error, std::size_t size) {
                self->context_.metrics_.traffic_out(size);
                if (error) {
                    self->close();
                } else {
                    self->read();
                }
            });
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
    std::array<char, 8192> read_buffer_;
    std::string request_bytes_;
    std::string response_bytes_;
    buffered_bytes_lease request_reservation_;
    buffered_bytes_lease response_reservation_;
    std::string client_address_;
    std::optional<connected_clients::address_key> client_key_;
    std::shared_ptr<const worker_execution_view> active_execution_;
    const v2::Website* website_{};
    std::optional<evaluated_route_policy> effective_route_;
    const evaluated_route_policy* route_{};
    origin_cursor origins_;
    std::uint64_t origin_attempt_{};
    origin_health_target current_health_;
    std::uint32_t healthy_threshold_{1};
    std::uint32_t unhealthy_threshold_{1};
    std::chrono::seconds connect_timeout_{10};
    std::chrono::seconds read_timeout_{30};
    std::optional<prepared_origin_request> prepared_request_;
    std::shared_ptr<origin_exchange> exchange_;
    std::string_view accept_encoding_;
    std::string relay_bytes_;
    std::unique_ptr<tunnel_state> tunnel_state_;
    std::uint64_t proxied_response_bytes_{};
    std::uint64_t sequence_{};
    bool secure_{};
    bool buffered_{};
    bool cache_prewarm_{};
    bool count_connection_{true};
    bool cache_stored_{};
    std::function<void(bool, std::string)> prewarm_completion_{};
    std::optional<cache_ticket> cache_ticket_{};
    std::optional<flexedge::cache::rule> cache_rule_{};
    std::optional<flexedge::cache::url> cache_url_{};
    cache_headers request_headers_{};
    unsigned cache_waits_{};
    std::shared_ptr<const cached_response> cached_response_{};
    std::ifstream cached_file_{};
    std::unique_ptr<cached_file_buffer> cached_buffer_;
    std::size_t cached_offset_{};
    std::uint32_t cached_status_{};
    bool cached_head_only_{};
    bool force_identity_encoding_{};
    bool upgrade_requested_{};
    bool tunnel_{};
    bool hsts_enabled_{};
    bool closed_{};
    bool continue_sent_{};
    bool response_pending_{};
};

}  // namespace flexedge::node
