#pragma once

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include "node/data/buffered_origin_exchange.h"
#include "node/data/origin_connection_pool.h"
#include "node/data/origin_response_codec.h"

namespace flexedge::node {

enum class StreamingOriginWriteStatus { kAccepted, kPaused, kRejected };

// Decodes an HTTP/1.1 chunked body incrementally.  SSE event payloads are emitted
// as soon as a complete wire fragment arrives; framing and trailers are never sent
// to the downstream HTTP/2 client.
class StreamingChunkedDecoder final {
  public:
    enum class Status { kIncomplete, kComplete, kInvalid };

    [[nodiscard]] Status consume(std::string_view input, std::string& output) {
        if (state_ == State::kComplete) {
            return input.empty() ? Status::kComplete : Status::kInvalid;
        }
        if (state_ == State::kInvalid) {
            return Status::kInvalid;
        }
        for (const auto ch : input) {
            if (!consumeByte(ch, output)) {
                state_ = State::kInvalid;
                return Status::kInvalid;
            }
        }
        return state_ == State::kComplete ? Status::kComplete : Status::kIncomplete;
    }

  private:
    static constexpr std::size_t kMaxFramingBytes = 64 * 1024;

    enum class State { kSizeLine, kBody, kDelimiter, kTrailers, kComplete, kInvalid };

    [[nodiscard]] bool consumeByte(char ch, std::string& output) {
        switch (state_) {
        case State::kSizeLine:
            if (!appendFraming(ch) || !line_.ends_with("\r\n")) {
                return line_.size() <= kMaxFramingBytes;
            }
            return consumeSizeLine();
        case State::kBody:
            output.push_back(ch);
            if (remaining_ == 0 || --remaining_ == 0) {
                delimiterOffset_ = 0;
                state_ = State::kDelimiter;
            }
            return true;
        case State::kDelimiter:
            if (ch != "\r\n"[delimiterOffset_]) {
                return false;
            }
            if (++delimiterOffset_ == 2) {
                state_ = State::kSizeLine;
            }
            return true;
        case State::kTrailers:
            if (!appendFraming(ch)) {
                return false;
            }
            if (line_ == "\r\n" || line_.ends_with("\r\n\r\n")) {
                state_ = State::kComplete;
            }
            return true;
        case State::kComplete:
        case State::kInvalid:
            return false;
        }
        return false;
    }

    [[nodiscard]] bool appendFraming(char ch) {
        if (line_.size() == kMaxFramingBytes) {
            return false;
        }
        line_.push_back(ch);
        return true;
    }

    [[nodiscard]] bool consumeSizeLine() {
        auto sizeText = std::string_view(line_).substr(0, line_.size() - 2);
        if (const auto extension = sizeText.find(';'); extension != std::string_view::npos) {
            sizeText = sizeText.substr(0, extension);
        }
        std::size_t size{};
        const auto parsed =
            std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size, 16);
        if (sizeText.empty() || parsed.ec != std::errc{} ||
            parsed.ptr != sizeText.data() + sizeText.size()) {
            return false;
        }
        line_.clear();
        if (size == 0) {
            state_ = State::kTrailers;
        } else {
            remaining_ = size;
            state_ = State::kBody;
        }
        return true;
    }

    State state_{State::kSizeLine};
    std::string line_;
    std::size_t remaining_{};
    std::size_t delimiterOffset_{};
};

// A one-way HTTP/1 origin response bridge for long-lived responses such as SSE.
// The response head is delivered immediately.  Body delivery obeys the downstream
// HTTP/2 flow-control callback, so a slow client cannot make this exchange buffer
// an unbounded event stream.
class StreamingOriginExchange final
    : public std::enable_shared_from_this<StreamingOriginExchange> {
  public:
    using HeadCallback = std::function<bool(BufferedProxyResponse)>;
    using DataCallback = std::function<StreamingOriginWriteStatus(std::string_view)>;
    using Completion = std::function<void(std::error_code)>;

    StreamingOriginExchange(const asio::any_io_executor& executor,
                            OriginConnectionPool& originConnections, std::string http1Wire,
                            ruvia::Http1ClientExchangeState responseExchange, std::string host,
                            std::uint16_t port, bool secure,
                            std::chrono::seconds connectTimeout,
                            std::chrono::seconds readTimeout, const v2::RouteRule* route,
                            HeadCallback head, DataCallback data, Completion completion)
        : originConnections_(originConnections), timer_(executor), http1Wire_(std::move(http1Wire)),
          host_(std::move(host)), port_(port), secure_(secure),
          originKey_{.host = host_, .port = port_, .secure = secure_},
          connectTimeout_(connectTimeout), readTimeout_(readTimeout),
          responseCodec_(std::make_unique<OriginResponseCodec>(std::move(responseExchange), false,
                                                               route)),
          head_(std::move(head)), data_(std::move(data)), completion_(std::move(completion)) {}

    void start() {
        armTimeout(connectTimeout_);
        startTransport();
    }

    void resume() {
        if (completed_ || !paused_) {
            return;
        }
        paused_ = false;
        finishOrRead();
    }

    void close() noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        head_ = {};
        data_ = {};
        completion_ = {};
        std::error_code ignored;
        timer_.cancel(ignored);
        discardTransport();
    }

  private:
    static bool normalOriginClose(const std::error_code& error) noexcept {
        return error == asio::error::eof || error == asio::ssl::error::stream_truncated;
    }

    void armTimeout(std::chrono::seconds timeout) {
        std::error_code ignored;
        timer_.cancel(ignored);
        timer_.expires_after(timeout);
        const auto self = shared_from_this();
        timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->finish(std::make_error_code(std::errc::timed_out));
            }
        });
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
                              self->armTimeout(self->readTimeout_);
                              self->readHttp1();
                          });
    }

    [[nodiscard]] bool retryStaleOriginTransport() {
        if (!reusedOriginTransport_ || retriedReusedOriginTransport_) {
            return false;
        }
        retriedReusedOriginTransport_ = true;
        discardTransport();
        armTimeout(connectTimeout_);
        startTransport();
        return true;
    }

    void readHttp1() {
        if (completed_ || paused_ || !transport_) {
            return;
        }
        const auto self = shared_from_this();
        transport_->read(
            asio::buffer(readBuffer_), [self](const std::error_code& error, std::size_t size) {
                self->readCompleted(error, size);
            });
    }

    void readCompleted(const std::error_code& error, std::size_t size) {
        if (completed_) {
            return;
        }
        if (size != 0) {
            armTimeout(readTimeout_);
            if (!consumeHttp1(std::string_view(readBuffer_.data(), size))) {
                finish(std::make_error_code(std::errc::protocol_error));
                return;
            }
        }
        if (paused_) {
            pendingReadError_ = error;
            return;
        }
        if (responseCodec_->complete()) {
            finish({});
            return;
        }
        if (error) {
            if (normalOriginClose(error) && headReady_ && responseCodec_->eofCompletesResponse()) {
                finish({});
            } else {
                finish(error);
            }
            return;
        }
        readHttp1();
    }

    [[nodiscard]] bool consumeHttp1(std::string_view input) {
        if (!headReady_) {
            const auto status = responseCodec_->consumeHead(input);
            if (status == OriginResponseHeadStatus::kFailure) {
                return false;
            }
            if (status == OriginResponseHeadStatus::kNeedMore) {
                return true;
            }
            auto output = responseCodec_->takeOutput();
            const auto parsed = detail::parseBufferedResponseHead(output);
            if (!parsed) {
                return false;
            }
            BufferedProxyResponse response;
            response.status = parsed->status;
            response.headers = std::move(parsed->headers);
            headReady_ = true;
            if (!head_ || !head_(std::move(response))) {
                return false;
            }
            return forwardBody(std::string_view(output).substr(parsed->bodyOffset));
        }
        if (!responseCodec_->consumeBody(input)) {
            return false;
        }
        return forwardBody(input);
    }

    [[nodiscard]] bool forwardBody(std::string_view wire) {
        std::string payload;
        if (responseCodec_->chunked()) {
            if (chunked_.consume(wire, payload) == StreamingChunkedDecoder::Status::kInvalid) {
                return false;
            }
        } else {
            payload.assign(wire);
        }
        if (payload.empty()) {
            return true;
        }
        if (!data_) {
            return false;
        }
        switch (data_(payload)) {
        case StreamingOriginWriteStatus::kAccepted:
            return true;
        case StreamingOriginWriteStatus::kPaused:
            paused_ = true;
            return true;
        case StreamingOriginWriteStatus::kRejected:
            return false;
        }
        return false;
    }

    void finishOrRead() {
        if (completed_) {
            return;
        }
        const auto error = std::exchange(pendingReadError_, {});
        if (responseCodec_->complete()) {
            finish({});
        } else if (error) {
            if (normalOriginClose(error) && headReady_ && responseCodec_->eofCompletesResponse()) {
                finish({});
            } else {
                finish(error);
            }
        } else {
            readHttp1();
        }
    }

    void discardTransport() noexcept {
        if (transport_) {
            transport_->close();
        }
        transport_.reset();
    }

    void finish(std::error_code error) {
        if (completed_) {
            return;
        }
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        discardTransport();
        auto completion = std::move(completion_);
        if (completion) {
            completion(error);
        }
    }

    OriginConnectionPool& originConnections_;
    asio::steady_timer timer_;
    std::string http1Wire_;
    std::string host_;
    std::uint16_t port_{};
    bool secure_{};
    OriginConnectionKey originKey_;
    std::chrono::seconds connectTimeout_;
    std::chrono::seconds readTimeout_;
    std::unique_ptr<OriginTransport> transport_;
    std::unique_ptr<OriginResponseCodec> responseCodec_;
    HeadCallback head_;
    DataCallback data_;
    Completion completion_;
    std::array<char, 16384> readBuffer_{};
    StreamingChunkedDecoder chunked_;
    std::error_code pendingReadError_;
    bool reusedOriginTransport_{};
    bool retriedReusedOriginTransport_{};
    bool headReady_{};
    bool paused_{};
    bool completed_{};
};

} // namespace flexedge::node
