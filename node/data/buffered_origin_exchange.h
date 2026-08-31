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

#include "node/data/chunked_body.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_request_codec.h"
#include "node/data/origin_response_codec.h"
#include "node/data/buffered_bytes_budget.h"

namespace flexedge::node {

struct BufferedProxyRequest final {
    std::string method;
    std::string target;
    std::string authority;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool hasBody{};
};

struct BufferedProxyResponse final {
    BufferedBytesLease reservation;
    std::uint16_t status{502};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

namespace detail {

struct BufferedResponseHead final {
    std::uint16_t status{};
    std::size_t bodyOffset{};
    bool chunked{};
    std::vector<std::pair<std::string, std::string>> headers;
};

struct BufferedResponseHeadBounds final {
    std::size_t headEnd{};
    std::size_t statusLineEnd{};
    std::uint16_t status{};
};

inline std::string_view bufferedHeaderValue(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    return value;
}

inline std::string_view bufferedConnectionToken(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

inline void appendBufferedConnectionTokens(std::string_view value,
                                           std::vector<std::string>& tokens) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        const auto token = bufferedConnectionToken(value.substr(0, comma));
        if (!token.empty()) {
            tokens.emplace_back(token);
        }
        if (comma == std::string_view::npos) {
            return;
        }
        value.remove_prefix(comma + 1);
    }
}

inline bool bufferedResponseHopByHop(std::string_view name) noexcept {
    return httpHeaderName(name, "Connection") || httpHeaderName(name, "Keep-Alive") ||
           httpHeaderName(name, "Proxy-Connection") || httpHeaderName(name, "Transfer-Encoding") ||
           httpHeaderName(name, "Upgrade") || httpHeaderName(name, "Trailer");
}

inline std::optional<std::uint16_t> parseBufferedResponseStatus(std::string_view line) noexcept {
    if (!line.starts_with("HTTP/")) {
        return std::nullopt;
    }
    const auto firstSpace = line.find(' ');
    if (firstSpace == std::string_view::npos || line.size() < firstSpace + 4) {
        return std::nullopt;
    }
    std::uint16_t status{};
    const auto parsed =
        std::from_chars(line.data() + firstSpace + 1, line.data() + firstSpace + 4, status);
    if (parsed.ec != std::errc{} || parsed.ptr != line.data() + firstSpace + 4) {
        return std::nullopt;
    }
    return status;
}

inline std::optional<BufferedResponseHeadBounds>
findBufferedResponseHead(std::string_view input) noexcept {
    auto headEnd = input.find("\r\n\r\n");
    if (headEnd == std::string_view::npos) {
        return std::nullopt;
    }
    auto headStart = std::size_t{};
    for (;;) {
        const auto statusLineEnd = input.find("\r\n", headStart);
        if (statusLineEnd == std::string_view::npos || statusLineEnd > headEnd) {
            return std::nullopt;
        }
        const auto status =
            parseBufferedResponseStatus(input.substr(headStart, statusLineEnd - headStart));
        if (!status) {
            return std::nullopt;
        }
        if (*status >= 200) {
            return BufferedResponseHeadBounds{
                .headEnd = headEnd, .statusLineEnd = statusLineEnd, .status = *status};
        }
        headStart = headEnd + 4;
        headEnd = input.find("\r\n\r\n", headStart);
        if (headEnd == std::string_view::npos) {
            return std::nullopt;
        }
    }
}

inline std::optional<BufferedResponseHead> parseBufferedResponseHead(std::string_view input) {
    const auto bounds = findBufferedResponseHead(input);
    if (!bounds) {
        return std::nullopt;
    }

    BufferedResponseHead result{.status = bounds->status,
                                .bodyOffset = bounds->headEnd + 4,
                                .chunked = false,
                                .headers = {}};
    std::vector<std::string> connectionTokens;
    auto cursor = bounds->statusLineEnd + 2;
    while (cursor < bounds->headEnd) {
        const auto lineEnd = input.find("\r\n", cursor);
        if (lineEnd == std::string_view::npos || lineEnd > bounds->headEnd) {
            return std::nullopt;
        }
        const auto line = input.substr(cursor, lineEnd - cursor);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos) {
            const auto name = line.substr(0, colon);
            const auto value = bufferedHeaderValue(line.substr(colon + 1));
            if (httpHeaderName(name, "Transfer-Encoding") &&
                value.find("chunked") != std::string_view::npos) {
                result.chunked = true;
            }
            if (httpHeaderName(name, "Connection")) {
                appendBufferedConnectionTokens(value, connectionTokens);
            }
            if (!bufferedResponseHopByHop(name)) {
                result.headers.emplace_back(name, value);
            }
        }
        cursor = lineEnd + 2;
    }

    result.headers.erase(std::remove_if(result.headers.begin(), result.headers.end(),
                                        [&](const auto& header) {
                                            return std::ranges::any_of(
                                                connectionTokens, [&](const auto& token) {
                                                    return httpHeaderName(header.first, token);
                                                });
                                        }),
                         result.headers.end());
    return result;
}

} // namespace detail

class BufferedOriginExchange final : public std::enable_shared_from_this<BufferedOriginExchange> {
  public:
    using Completion = std::function<void(std::error_code, BufferedProxyResponse)>;

    BufferedOriginExchange(const asio::any_io_executor& executor,
                           OriginConnectionPool& originConnections, std::string http1Wire,
                           ruvia::Http1ClientExchangeState responseExchange, std::string host,
                           std::uint16_t port, bool secure, std::chrono::seconds timeout,
                           BufferedBytesBudget& responseBuffers, Completion completion)
        : originConnections_(originConnections), timer_(executor), http1Wire_(std::move(http1Wire)),
          host_(std::move(host)), port_(port), secure_(secure),
          originKey_{.host = host_, .port = port_, .secure = secure_}, timeout_(timeout),
          responseCodec_(std::make_unique<OriginResponseCodec>(std::move(responseExchange), false)),
          completion_(std::move(completion)), reservation_(responseBuffers.lease()) {}

    void start() {
        const auto self = shared_from_this();
        timer_.expires_after(timeout_);
        timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->finish(std::make_error_code(std::errc::timed_out));
            }
        });
        startTransport();
    }

    void close() noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        completion_ = {};
        std::error_code ignored;
        timer_.cancel(ignored);
        discardTransport();
    }

  private:
    static constexpr std::size_t kMaxResponseBytes = 64 * 1024 * 1024;

    static bool normalOriginClose(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
    }

    void startTransport() {
        if (completed_) {
            return;
        }
        transport_ = originConnections_.acquire(originKey_);
        reusedOriginTransport_ = transport_->connected();
        if (reusedOriginTransport_) {
            startHttp1();
            return;
        }
        const auto self = shared_from_this();
        transport_->connect(host_, port_, secure_, false,
                            [self](const std::error_code& error, HttpWireProtocol) {
                                if (error) {
                                    self->finish(error);
                                } else {
                                    self->startHttp1();
                                }
                            });
    }

    void startHttp1() {
        if (completed_ || !transport_) {
            return;
        }
        const auto self = shared_from_this();
        transport_->write(asio::buffer(http1Wire_),
                          [self](const std::error_code& error, std::size_t size) {
                              if (error) {
                                  if (size == 0 && self->retryStaleOriginTransport()) {
                                      return;
                                  }
                                  self->finish(error);
                                  return;
                              }
                              self->readHttp1();
                          });
    }

    bool retryStaleOriginTransport() {
        if (!reusedOriginTransport_ || retriedReusedOriginTransport_) {
            return false;
        }
        retriedReusedOriginTransport_ = true;
        discardTransport();
        startTransport();
        return true;
    }

    void readHttp1() {
        if (completed_ || !transport_) {
            return;
        }
        const auto self = shared_from_this();
        transport_->read(
            asio::buffer(readBuffer_), [self](const std::error_code& error, std::size_t size) {
                if (self->completed_) {
                    return;
                }
                if (size != 0) {
                    if (self->input_.size() + size > kMaxResponseBytes) {
                        self->finish(std::make_error_code(std::errc::message_size));
                        return;
                    }
                    if (!self->reservation_.tryGrow(size)) {
                        self->finish(std::make_error_code(std::errc::not_enough_memory));
                        return;
                    }
                    self->input_.append(self->readBuffer_.data(), size);
                    if (!self->consumeHttp1(std::string_view(self->readBuffer_.data(), size))) {
                        self->finish(std::make_error_code(std::errc::protocol_error));
                        return;
                    }
                }
                if (self->responseCodec_->complete()) {
                    self->finishHttp1(true);
                    return;
                }
                if (error) {
                    if (normalOriginClose(error) && self->headReady_ &&
                        self->responseCodec_->eofCompletesResponse()) {
                        self->finishHttp1(false);
                        return;
                    }
                    self->finish(error);
                    return;
                }
                self->readHttp1();
            });
    }

    bool consumeHttp1(std::string_view bytes) {
        if (!headReady_) {
            const auto status = responseCodec_->consumeHead(bytes);
            if (status == OriginResponseHeadStatus::kFailure) {
                return false;
            }
            if (status == OriginResponseHeadStatus::kNeedMore) {
                return true;
            }
            (void)responseCodec_->takeOutput();
            headReady_ = true;
            return true;
        }
        return responseCodec_->consumeBody(bytes);
    }

    void finishHttp1(bool transportReusable) {
        if (completed_) {
            return;
        }
        transportReusable_ = transportReusable;
        auto parsed = detail::parseBufferedResponseHead(input_);
        if (!parsed) {
            finish(std::make_error_code(std::errc::protocol_error));
            return;
        }
        response_.status = parsed->status;
        response_.headers = std::move(parsed->headers);
        const auto body = std::string_view(input_).substr(parsed->bodyOffset);
        if (parsed->chunked) {
            if (!reservation_.tryGrow(body.size())) {
                finish(std::make_error_code(std::errc::not_enough_memory));
                return;
            }
            auto decoded = decodeChunkedBody(body, kMaxResponseBytes);
            if (!decoded) {
                reservation_.shrink(body.size());
                finish(std::make_error_code(std::errc::protocol_error));
                return;
            }
            response_.body = std::move(*decoded);
            reservation_.shrink(input_.size());
            input_.clear();
            input_.shrink_to_fit();
            reservation_.shrink(body.size() - response_.body.size());
        } else {
            input_.erase(0, parsed->bodyOffset);
            response_.body = std::move(input_);
        }
        finish({});
    }

    void discardTransport() noexcept {
        if (transport_) {
            transport_->close();
        }
        transport_.reset();
    }

    void releaseTransport() noexcept {
        if (transport_ && transportReusable_ && responseCodec_->originReusable()) {
            originConnections_.release(std::move(originKey_), std::move(transport_));
        } else {
            discardTransport();
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
            discardTransport();
        } else {
            releaseTransport();
        }
        auto completion = std::move(completion_);
        response_.reservation = std::move(reservation_);
        if (completion) {
            completion(error, std::move(response_));
        }
    }

    OriginConnectionPool& originConnections_;
    asio::steady_timer timer_;
    std::string http1Wire_;
    std::string host_;
    std::uint16_t port_{};
    bool secure_{};
    OriginConnectionKey originKey_;
    std::chrono::seconds timeout_;
    std::unique_ptr<OriginResponseCodec> responseCodec_;
    Completion completion_;
    std::array<char, 16384> readBuffer_{};
    std::string input_;
    BufferedProxyResponse response_;
    BufferedBytesLease reservation_;
    std::unique_ptr<OriginTransport> transport_;
    bool reusedOriginTransport_{};
    bool retriedReusedOriginTransport_{};
    bool headReady_{};
    bool transportReusable_{};
    bool completed_{};
};

} // namespace flexedge::node
