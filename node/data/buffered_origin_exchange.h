#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>

#include "node/data/buffered_bytes_budget.h"
#include "node/data/chunked_body.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_response_codec.h"

namespace flexedge::node {

struct buffered_proxy_request final {
    std::string method_;
    std::string target_;
    std::string authority_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
    bool has_body_{};
};

struct buffered_proxy_response final {
    buffered_bytes_lease reservation_;
    std::uint16_t status_{502};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

namespace detail {

struct buffered_response_head final {
    std::uint16_t status_{};
    std::size_t body_offset_{};
    bool chunked_{};
    std::vector<std::pair<std::string, std::string>> headers_;
};

struct buffered_response_head_bounds final {
    std::size_t head_end_{};
    std::size_t status_line_end_{};
    std::uint16_t status_{};
};

inline std::string_view buffered_header_value(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    return value;
}

inline std::string_view buffered_connection_token(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

inline void append_buffered_connection_tokens(std::string_view value,
    std::vector<std::string>& tokens) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = buffered_connection_token(value.substr(0, comma));
        if (!token.empty()) {
            tokens.emplace_back(token);
        }
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

inline bool buffered_response_hop_by_hop(std::string_view name) noexcept {
    return http_header_name(name, "Connection") || http_header_name(name, "Keep-Alive") ||
           http_header_name(name, "Proxy-Connection") || http_header_name(name, "Transfer-Encoding") ||
           http_header_name(name, "Upgrade") || http_header_name(name, "Trailer");
}

inline std::optional<std::uint16_t> parse_buffered_response_status(std::string_view line) noexcept {
    if (!line.starts_with("HTTP/")) {
        return std::nullopt;
    }
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos || line.size() < first_space + 4) {
        return std::nullopt;
    }
    std::uint16_t status{};
    const auto parsed =
        std::from_chars(line.data() + first_space + 1, line.data() + first_space + 4, status);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + first_space + 4) {
        return std::nullopt;
    }
    return status;
}

inline std::optional<buffered_response_head_bounds>
find_buffered_response_head(std::string_view input) noexcept {
    auto head_end = input.find("\r\n\r\n");
    if (head_end == std::string_view::npos) {
        return std::nullopt;
    }
    auto head_start = std::size_t{};
    for (;;) {
        const auto status_line_end = input.find("\r\n", head_start);
        if (status_line_end == std::string_view::npos || status_line_end > head_end) {
            return std::nullopt;
        }
        const auto status =
            parse_buffered_response_status(input.substr(head_start, status_line_end - head_start));
        if (!status) {
            return std::nullopt;
        }
        if (*status >= 200) {
            return buffered_response_head_bounds{
                .head_end_ = head_end, .status_line_end_ = status_line_end, .status_ = *status};
        }
        head_start = head_end + 4;
        head_end = input.find("\r\n\r\n", head_start);
        if (head_end == std::string_view::npos) {
            return std::nullopt;
        }
    }
}

inline std::optional<buffered_response_head> parse_buffered_response_head(std::string_view input) {
    const auto bounds = find_buffered_response_head(input);
    if (!bounds) {
        return std::nullopt;
    }

    buffered_response_head result{.status_ = bounds->status_,
        .body_offset_ = bounds->head_end_ + 4,
        .chunked_ = false,
        .headers_ = {}};
    std::vector<std::string> connection_tokens;
    auto cursor = bounds->status_line_end_ + 2;
    while (cursor < bounds->head_end_) {
        const auto line_end = input.find("\r\n", cursor);
        if (line_end == std::string_view::npos || line_end > bounds->head_end_) {
            return std::nullopt;
        }
        const auto line = input.substr(cursor, line_end - cursor);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto name = line.substr(0, colon);
            const auto value = buffered_header_value(line.substr(colon + 1));
            if (http_header_name(name, "Transfer-Encoding") &&
                value.find("chunked") != std::string_view::npos) {
                result.chunked_ = true;
            }
            if (http_header_name(name, "Connection")) {
                append_buffered_connection_tokens(value, connection_tokens);
            }
            if (!buffered_response_hop_by_hop(name)) {
                result.headers_.emplace_back(name, value);
            }
        }
        cursor = line_end + 2;
    }

    result.headers_.erase(std::remove_if(result.headers_.begin(), result.headers_.end(),
                              [&](const auto& header) {
                                  return std::ranges::any_of(
                                      connection_tokens, [&](const auto& token) {
                                          return http_header_name(header.first, token);
                                      });
                              }),
        result.headers_.end());
    return result;
}

}  // namespace detail

class buffered_origin_exchange final : public std::enable_shared_from_this<buffered_origin_exchange> {
public:
    using completion = std::function<void(std::error_code, buffered_proxy_response)>;

    buffered_origin_exchange(const asio::any_io_executor& executor,
        origin_connection_pool& origin_connections, std::string http1_wire,
        ruvia::http1_client_exchange_state response_exchange, std::string host,
        std::uint16_t port, bool secure, std::chrono::seconds timeout,
        buffered_bytes_budget& response_buffers, completion completion)
        : origin_connections_(origin_connections),
          timer_(executor),
          http1_wire_(std::move(http1_wire)),
          host_(std::move(host)),
          port_(port),
          secure_(secure),
          origin_key_{.host_ = host_, .port_ = port_, .secure_ = secure_},
          timeout_(timeout),
          response_codec_(std::make_unique<origin_response_codec>(std::move(response_exchange), false)),
          completion_(std::move(completion)),
          reservation_(response_buffers.lease()) {}

    void start() {
        const auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->finish(std::make_error_code(std::errc::timed_out));
            }
        });
        start_transport();
    }

    void close() noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        completion_ = {};
        std::error_code ignored;
        timer_.cancel(ignored);
        discard_transport();
    }

private:
    static constexpr std::size_t max_response_bytes = 64 * 1024 * 1024;

    static bool normal_origin_close(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
    }

    void start_transport() {
        if (completed_) {
            return;
        }
        transport_ = origin_connections_.acquire(origin_key_);
        reused_origin_transport_ = transport_->connected();
        if (reused_origin_transport_) {
            start_http1();
            return;
        }
        const auto self = shared_from_this();
        transport_->connect(host_, port_, secure_,
            [self](const std::error_code& error) {
                if (error) {
                    self->finish(error);
                } else {
                    self->start_http1();
                }
            });
    }

    void start_http1() {
        if (completed_ || !transport_) {
            return;
        }
        const auto self = shared_from_this();
        transport_->write(asio::buffer(http1_wire_),
            [self](const std::error_code& error, std::size_t size) {
                if (error) {
                    if (size == 0 && self->retry_stale_origin_transport()) {
                        return;
                    }
                    self->finish(error);
                    return;
                }
                self->read_http1();
            });
    }

    bool retry_stale_origin_transport() {
        if (!reused_origin_transport_ || retried_reused_origin_transport_) {
            return false;
        }
        retried_reused_origin_transport_ = true;
        discard_transport();
        start_transport();
        return true;
    }

    void read_http1() {
        if (completed_ || !transport_) {
            return;
        }
        const auto self = shared_from_this();
        transport_->read(
            asio::buffer(read_buffer_), [self](const std::error_code& error, std::size_t size) {
                if (self->completed_) {
                    return;
                }
                if (size != 0) {
                    if (self->input_.size() + size > max_response_bytes) {
                        self->finish(std::make_error_code(std::errc::message_size));
                        return;
                    }
                    if (!self->reservation_.try_grow(size)) {
                        self->finish(std::make_error_code(std::errc::not_enough_memory));
                        return;
                    }
                    self->input_.append(self->read_buffer_.data(), size);
                    if (!self->consume_http1(std::string_view(self->read_buffer_.data(), size))) {
                        self->finish(std::make_error_code(std::errc::protocol_error));
                        return;
                    }
                }
                if (self->response_codec_->complete()) {
                    self->finish_http1(true);
                    return;
                }
                if (error) {
                    if (normal_origin_close(error) && self->head_ready_ &&
                        self->response_codec_->eof_completes_response()) {
                        self->finish_http1(false);
                        return;
                    }
                    self->finish(error);
                    return;
                }
                self->read_http1();
            });
    }

    bool consume_http1(std::string_view bytes) {
        if (!head_ready_) {
            const auto status = response_codec_->consume_head(bytes);
            if (status == origin_response_head_status::failure) {
                return false;
            }
            if (status == origin_response_head_status::need_more) {
                return true;
            }
            (void)response_codec_->take_output();
            head_ready_ = true;
            return true;
        }
        return response_codec_->consume_body(bytes);
    }

    void finish_http1(bool transport_reusable) {
        if (completed_) {
            return;
        }
        transport_reusable_ = transport_reusable;
        auto parsed = detail::parse_buffered_response_head(input_);
        if (!parsed) {
            finish(std::make_error_code(std::errc::protocol_error));
            return;
        }
        response_.status_ = parsed->status_;
        response_.headers_ = std::move(parsed->headers_);
        const auto body = std::string_view(input_).substr(parsed->body_offset_);
        if (parsed->chunked_) {
            if (!reservation_.try_grow(body.size())) {
                finish(std::make_error_code(std::errc::not_enough_memory));
                return;
            }
            auto decoded = decode_chunked_body(body, max_response_bytes);
            if (!decoded) {
                reservation_.shrink(body.size());
                finish(std::make_error_code(std::errc::protocol_error));
                return;
            }
            response_.body_ = std::move(*decoded);
            reservation_.shrink(input_.size());
            input_.clear();
            input_.shrink_to_fit();
            reservation_.shrink(body.size() - response_.body_.size());
        } else {
            input_.erase(0, parsed->body_offset_);
            response_.body_ = std::move(input_);
        }
        finish({});
    }

    void discard_transport() noexcept {
        if (transport_) {
            transport_->close();
        }
        transport_.reset();
    }

    void release_transport() noexcept {
        if (transport_ && transport_reusable_ && response_codec_->origin_reusable()) {
            origin_connections_.release(std::move(origin_key_), std::move(transport_));
        } else {
            discard_transport();
        }
    }

    void finish(std::error_code error) {
        if (completed_) {
            return;
        }
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        if (error) {
            discard_transport();
        } else {
            release_transport();
        }
        auto completion = std::move(completion_);
        response_.reservation_ = std::move(reservation_);
        if (completion) {
            completion(error, std::move(response_));
        }
    }

    origin_connection_pool& origin_connections_;
    asio::steady_timer timer_;
    std::string http1_wire_;
    std::string host_;
    std::uint16_t port_{};
    bool secure_{};
    origin_connection_key origin_key_;
    std::chrono::seconds timeout_;
    std::unique_ptr<origin_response_codec> response_codec_;
    completion completion_;
    std::array<char, 16384> read_buffer_{};
    std::string input_;
    buffered_proxy_response response_;
    buffered_bytes_lease reservation_;
    std::unique_ptr<origin_transport> transport_;
    bool reused_origin_transport_{};
    bool retried_reused_origin_transport_{};
    bool head_ready_{};
    bool transport_reusable_{};
    bool completed_{};
};

}  // namespace flexedge::node
