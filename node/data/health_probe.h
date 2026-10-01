#pragma once

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
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
    OriginHealthProbe(ruvia::EventLoop loop, OriginHealthRegistry::Target target,
                      OriginTlsContext& tlsContext, OriginProbeConfig config,
                      std::function<void()> onRetired = {})
        : loop_(std::move(loop)), target_(std::move(target)), tlsContext_(tlsContext),
          config_(std::move(config)), onRetired_(std::move(onRetired)),
          resolver_(loop_.ioContext()), socket_(loop_.ioContext()), timer_(loop_.ioContext()) {}

    void setOnRetired(std::function<void()> callback) {
        onRetired_ = std::move(callback);
    }

    void cancel() noexcept {
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        resolver_.cancel();
        if (tlsStream_) ignored = tlsStream_->lowest_layer().close(ignored);
        ignored = socket_.close(ignored);
        notifyRetiredIfReady();
    }

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
        const auto self = shared_from_this();
        try {
            timer_.expires_after(config_.timeout);
            operationStarted();
            try {
                timer_.async_wait([self](const std::error_code& error) {
                    Completion completion{self};
                    if (!error) self->complete(false, "timeout");
                });
            } catch (...) {
                operationCompleted();
                throw;
            }
            operationStarted();
            try {
                resolver_.async_resolve(config_.host, std::to_string(config_.port),
                                        [self](const std::error_code& error,
                                               const asio::ip::tcp::resolver::results_type& endpoints) {
                                            Completion completion{self};
                                            if (self->completed_) return;
                                            if (error) {
                                                self->complete(false, "dns lookup failed");
                                                return;
                                            }
                                            self->connect(endpoints);
                                        });
            } catch (...) {
                operationCompleted();
                throw;
            }
        } catch (const std::exception& error) {
            log("async probe startup failed", error);
            complete(false, "async probe startup failed");
        } catch (...) {
            log("async probe startup failed with unknown exception");
            complete(false, "async probe startup failed");
        }
    }

  private:
    struct Completion final {
        std::shared_ptr<OriginHealthProbe> probe;
        ~Completion() {
            try {
                asio::post(probe->loop_.executor(), [keepAlive = probe] {
                    keepAlive->operationCompleted();
                });
            } catch (...) {
                probe->operationCompleted();
            }
        }
    };

    void operationStarted() noexcept { ++pendingOperations_; }

    template <typename Initiation>
    void initiateOperation(Initiation&& initiation, std::string_view failure) noexcept {
        operationStarted();
        try {
            std::forward<Initiation>(initiation)();
        } catch (const std::exception& error) {
            operationCompleted();
            log(failure, error);
            complete(false, failure);
        } catch (...) {
            operationCompleted();
            log(failure);
            complete(false, failure);
        }
    }

    void operationCompleted() noexcept {
        --pendingOperations_;
        notifyRetiredIfReady();
    }

    void notifyRetiredIfReady() noexcept {
        if (!completed_ || pendingOperations_ != 0 || retiredNotified_) return;
        retiredNotified_ = true;
        if (onRetired_) {
            try { onRetired_(); } catch (...) {}
        }
    }

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
        initiateOperation(
            [&] {
                asio::async_connect(socket_, endpoints,
                                    [self](const std::error_code& error,
                                           const asio::ip::tcp::endpoint&) {
                                        Completion completion{self};
                                        if (self->completed_) return;
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
            },
            "connection initiation failed");
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
        initiateOperation(
            [&] {
                tlsStream_->async_handshake(asio::ssl::stream_base::client,
                                            [self](const std::error_code& error) {
                                                Completion completion{self};
                                                if (self->completed_) return;
                                                if (error) {
                                                    self->complete(false, "TLS handshake failed");
                                                    return;
                                                }
                                                self->write();
                                            });
            },
            "TLS handshake initiation failed");
    }

    void write() {
        const auto self = shared_from_this();
        initiateOperation(
            [&] {
                auto completion = [self](const std::error_code& error, std::size_t) {
                    Completion operation{self};
                    if (self->completed_) return;
                    if (error) {
                        self->complete(false, "request write failed");
                        return;
                    }
                    self->read();
                };
                if (tlsStream_) {
                    asio::async_write(*tlsStream_, asio::buffer(requestBytes_),
                                      std::move(completion));
                } else {
                    asio::async_write(socket_, asio::buffer(requestBytes_),
                                      std::move(completion));
                }
            },
            "request write initiation failed");
    }

    void read() {
        const auto self = shared_from_this();
        initiateOperation(
            [&] {
                auto completion = [self](const std::error_code& error, std::size_t size) {
                    Completion operation{self};
                    if (self->completed_) return;
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
            },
            "response read initiation failed");
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
        target_.recordProbe(
            healthy,
            healthy ? config_.healthyThreshold : config_.unhealthyThreshold,
            static_cast<std::uint32_t>((std::min)(elapsed.count(), std::int64_t{600000})), error);
        notifyRetiredIfReady();
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
    OriginHealthRegistry::Target target_;
    OriginTlsContext& tlsContext_;
    OriginProbeConfig config_;
    std::function<void()> onRetired_;
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
    std::size_t pendingOperations_{};
    bool completed_{};
    bool retiredNotified_{};
};

} // namespace flexedge::node
