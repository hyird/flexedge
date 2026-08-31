#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <ruvia/core/EventLoop.h>
#include <ruvia/http/Http1ClientRequestWriter.h>
#include <ruvia/http/Http1ClientResponseParser.h>

#include "node/data/origin_health.h"
#include "node/data/origin_tls.h"

namespace flexedge::node {

struct OriginProbeConfig final {
    std::string websiteId;
    std::string originId;
    std::string protocol;
    std::string host;
    std::uint16_t port{};
    std::string path{"/"};
    std::chrono::seconds timeout{3};
    std::uint32_t expectedStatus{200};
    std::uint32_t healthyThreshold{2};
    std::uint32_t unhealthyThreshold{3};
};

class OriginHealthProbe final : public std::enable_shared_from_this<OriginHealthProbe> {
  public:
    OriginHealthProbe(ruvia::EventLoop loop, OriginHealthRegistry& registry,
                      OriginTlsContext& tlsContext, OriginProbeConfig config)
        : loop_(std::move(loop)), registry_(registry), tlsContext_(tlsContext),
          config_(std::move(config)), resolver_(loop_.ioContext()), socket_(loop_.ioContext()),
          timer_(loop_.ioContext()) {}

    void start() {
        try {
            prepareRequest();
        } catch (const std::exception& error) {
            log("request preparation failed", error);
            complete(false, "request preparation failed");
            return;
        } catch (...) {
            log("request preparation failed with unknown exception");
            complete(false, "request preparation failed");
            return;
        }
        timer_.expires_after(config_.timeout);
        const auto self = shared_from_this();
        timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->complete(false, "timeout");
            }
        });
        resolver_.async_resolve(config_.host, std::to_string(config_.port),
                                [self](const std::error_code& error,
                                       const asio::ip::tcp::resolver::results_type& endpoints) {
                                    if (error) {
                                        self->complete(false, "dns lookup failed");
                                        return;
                                    }
                                    self->connect(endpoints);
                                });
    }

  private:
    void prepareRequest() {
        const auto origin =
            config_.protocol == "https"
                ? ruvia::HttpOriginView::https({.host = config_.host, .port = config_.port})
                : ruvia::HttpOriginView::http({.host = config_.host, .port = config_.port});
        ruvia::HttpClientRequestView request;
        request.method = "HEAD";
        request.target = config_.path;
        std::array<char, 4096> head{};
        auto prepared = ruvia::Http1ClientRequestWriter().prepare(
            origin, request, head, {.closePolicy = ruvia::Http1ClosePolicy::kCloseAfterResponse});
        if (!prepared.prepared()) {
            throw std::runtime_error("could not prepare origin health request");
        }
        requestBytes_.assign(prepared.prepared()->head());
        responseParser_ = std::make_unique<ruvia::Http1ClientResponseParser>(
            prepared.prepared()->exchangeState());
    }

    void connect(const asio::ip::tcp::resolver::results_type& endpoints) {
        const auto self = shared_from_this();
        asio::async_connect(socket_, endpoints,
                            [self](const std::error_code& error, const asio::ip::tcp::endpoint&) {
                                if (error) {
                                    self->complete(false, "connection failed");
                                    return;
                                }
                                if (self->config_.protocol == "https") {
                                    self->handshake();
                                } else {
                                    self->write();
                                }
                            });
    }

    void handshake() {
        try {
            tlsStream_.emplace(tlsContext_.stream(std::move(socket_), config_.host, false));
        } catch (const std::exception& error) {
            log("TLS stream preparation failed", error);
            complete(false, "TLS setup failed");
            return;
        } catch (...) {
            log("TLS stream preparation failed with unknown exception");
            complete(false, "TLS setup failed");
            return;
        }
        const auto self = shared_from_this();
        tlsStream_->async_handshake(asio::ssl::stream_base::client,
                                    [self](const std::error_code& error) {
                                        if (error) {
                                            self->complete(false, "TLS handshake failed");
                                            return;
                                        }
                                        self->write();
                                    });
    }

    void write() {
        const auto self = shared_from_this();
        auto completion = [self](const std::error_code& error, std::size_t) {
            if (error) {
                self->complete(false, "request write failed");
                return;
            }
            self->read();
        };
        if (tlsStream_) {
            asio::async_write(*tlsStream_, asio::buffer(requestBytes_), std::move(completion));
        } else {
            asio::async_write(socket_, asio::buffer(requestBytes_), std::move(completion));
        }
    }

    void read() {
        const auto self = shared_from_this();
        auto completion = [self](const std::error_code& error, std::size_t size) {
            if (error) {
                self->complete(false, "response read failed");
                return;
            }
            self->responseBytes_.append(self->buffer_.data(), size);
            self->parse();
        };
        if (tlsStream_) {
            tlsStream_->async_read_some(asio::buffer(buffer_), std::move(completion));
        } else {
            socket_.async_read_some(asio::buffer(buffer_), std::move(completion));
        }
    }

    void parse() {
        for (;;) {
            auto result =
                responseParser_->parse(std::string_view(responseBytes_).substr(responseOffset_));
            if (result.needMore()) {
                if (responseBytes_.size() >= 65536) {
                    complete(false, "response too large");
                } else {
                    read();
                }
                return;
            }
            const auto* parsed = result.parsed();
            if (parsed == nullptr) {
                complete(false, "invalid response");
                return;
            }
            responseOffset_ += parsed->consumedBytes();
            if (parsed->plan().informational()) {
                continue;
            }
            const auto status = static_cast<std::uint32_t>(parsed->head().status().value());
            complete(status == config_.expectedStatus,
                     status == config_.expectedStatus
                         ? ""
                         : "unexpected status " + std::to_string(status));
            return;
        }
    }

    void complete(bool healthy, std::string_view error = {}) noexcept {
        if (completed_) {
            return;
        }
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        resolver_.cancel();
        if (tlsStream_) {
            ignored = tlsStream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startedAt_);
        registry_.recordProbe(
            config_.websiteId, config_.originId, healthy,
            healthy ? config_.healthyThreshold : config_.unhealthyThreshold,
            static_cast<std::uint32_t>((std::min)(elapsed.count(), std::int64_t{600000})), error);
    }

    void log(std::string_view message) const noexcept {
        try {
            std::cerr << "flexedge node health probe " << config_.host << ':' << config_.port
                      << ": " << message << '\n';
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error) const noexcept {
        try {
            std::cerr << "flexedge node health probe " << config_.host << ':' << config_.port
                      << ": " << message << ": " << error.what() << '\n';
        } catch (...) {
        }
    }

    ruvia::EventLoop loop_;
    OriginHealthRegistry& registry_;
    OriginTlsContext& tlsContext_;
    OriginProbeConfig config_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    std::optional<OriginTlsContext::Stream> tlsStream_;
    asio::steady_timer timer_;
    std::string requestBytes_;
    std::unique_ptr<ruvia::Http1ClientResponseParser> responseParser_;
    std::array<char, 4096> buffer_{};
    std::string responseBytes_;
    std::size_t responseOffset_{};
    std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};
    bool completed_{};
};

} // namespace flexedge::node
