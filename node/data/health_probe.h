#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/http/http1_client_request_writer.h>
#include <ruvia/http/http1_client_response_parser.h>
#include <ruvia/http/http1_close_policy.h>
#include <ruvia/http/http_client.h>

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

#include "node/data/origin_health.h"
#include "node/data/origin_tls.h"

namespace flexedge::node {

struct origin_probe_config final {
    std::string protocol_;
    std::string host_;
    std::uint16_t port_{};
    std::string path_{"/"};
    std::chrono::seconds timeout_{3};
    std::uint32_t expected_status_{200};
    std::uint32_t healthy_threshold_{2};
    std::uint32_t unhealthy_threshold_{3};
};

class origin_health_probe final : public std::enable_shared_from_this<origin_health_probe> {
public:
    origin_health_probe(ruvia::event_loop loop, origin_health_registry::target target,
        origin_tls_context& tls_context, origin_probe_config config,
        std::function<void()> on_retired = {})
        : loop_(std::move(loop)),
          target_(std::move(target)),
          tls_context_(tls_context),
          config_(std::move(config)),
          on_retired_(std::move(on_retired)),
          resolver_(loop_.io_context()),
          socket_(loop_.io_context()),
          timer_(loop_.io_context()) {}

    void set_on_retired(std::function<void()> callback) {
        on_retired_ = std::move(callback);
    }

    void cancel() noexcept {
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        resolver_.cancel();
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
        notify_retired_if_ready();
    }

    void start() {
        try {
            prepare_request();
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
            timer_.expires_after(config_.timeout_);
            operation_started();
            try {
                timer_.async_wait([self](const std::error_code& error) {
                    completion completion{self};
                    if (!error) {
                        self->complete(false, "timeout");
                    }
                });
            } catch (...) {
                operation_completed();
                throw;
            }
            operation_started();
            try {
                resolver_.async_resolve(config_.host_, std::to_string(config_.port_),
                    [self](const std::error_code& error,
                        const asio::ip::tcp::resolver::results_type& endpoints) {
                        completion completion{self};
                        if (self->completed_) {
                            return;
                        }
                        if (error) {
                            self->complete(false, "dns lookup failed");
                            return;
                        }
                        self->connect(endpoints);
                    });
            } catch (...) {
                operation_completed();
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
    struct completion final {
        std::shared_ptr<origin_health_probe> probe_;
        ~completion() {
            try {
                asio::post(probe_->loop_.executor(), [keep_alive = probe_] {
                    keep_alive->operation_completed();
                });
            } catch (...) {
                probe_->operation_completed();
            }
        }
    };

    void operation_started() noexcept {
        ++pending_operations_;
    }

    template <typename initiation_type>
    void initiate_operation(initiation_type&& initiation, std::string_view failure) noexcept {
        operation_started();
        try {
            std::forward<initiation_type>(initiation)();
        } catch (const std::exception& error) {
            operation_completed();
            log(failure, error);
            complete(false, failure);
        } catch (...) {
            operation_completed();
            log(failure);
            complete(false, failure);
        }
    }

    void operation_completed() noexcept {
        --pending_operations_;
        notify_retired_if_ready();
    }

    void notify_retired_if_ready() noexcept {
        if (!completed_ || pending_operations_ != 0 || retired_notified_) {
            return;
        }
        retired_notified_ = true;
        if (on_retired_) {
            try {
                on_retired_();
            } catch (...) {
            }
        }
    }

    void prepare_request() {
        const auto origin =
            config_.protocol_ == "https"
                ? ruvia::http_origin_view::https({.host_ = config_.host_, .port_ = config_.port_})
                : ruvia::http_origin_view::http({.host_ = config_.host_, .port_ = config_.port_});
        ruvia::http_client_request_view request;
        request.method_ = "HEAD";
        request.target_ = config_.path_;
        std::array<char, 4096> head{};
        auto prepared = ruvia::http1_client_request_writer().prepare(
            origin, request, head, {.close_policy_ = ruvia::http1_close_policy::close_after_response});
        if (!prepared.prepared()) {
            throw std::runtime_error("could not prepare origin health request");
        }
        request_bytes_.assign(prepared.prepared()->head());
        response_parser_ = std::make_unique<ruvia::http1_client_response_parser>(
            prepared.prepared()->exchange_state());
    }

    void connect(const asio::ip::tcp::resolver::results_type& endpoints) {
        const auto self = shared_from_this();
        initiate_operation(
            [&] {
                asio::async_connect(socket_, endpoints,
                    [self](const std::error_code& error,
                        const asio::ip::tcp::endpoint&) {
                        completion completion{self};
                        if (self->completed_) {
                            return;
                        }
                        if (error) {
                            self->complete(false, "connection failed");
                            return;
                        }
                        if (self->config_.protocol_ == "https") {
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
            tls_stream_.emplace(tls_context_.get_stream(std::move(socket_), config_.host_));
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
        initiate_operation(
            [&] {
                tls_stream_->async_handshake(asio::ssl::stream_base::client,
                    [self](const std::error_code& error) {
                        completion completion{self};
                        if (self->completed_) {
                            return;
                        }
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
        initiate_operation(
            [&] {
                auto completion_value = [self](const std::error_code& error, std::size_t) {
                    completion operation{self};
                    if (self->completed_) {
                        return;
                    }
                    if (error) {
                        self->complete(false, "request write failed");
                        return;
                    }
                    self->read();
                };
                if (tls_stream_) {
                    asio::async_write(*tls_stream_, asio::buffer(request_bytes_),
                        std::move(completion_value));
                } else {
                    asio::async_write(socket_, asio::buffer(request_bytes_),
                        std::move(completion_value));
                }
            },
            "request write initiation failed");
    }

    void read() {
        const auto self = shared_from_this();
        initiate_operation(
            [&] {
                auto completion_value = [self](const std::error_code& error, std::size_t size) {
                    completion operation{self};
                    if (self->completed_) {
                        return;
                    }
                    if (error) {
                        self->complete(false, "response read failed");
                        return;
                    }
                    self->response_bytes_.append(self->buffer_.data(), size);
                    self->parse();
                };
                if (tls_stream_) {
                    tls_stream_->async_read_some(asio::buffer(buffer_), std::move(completion_value));
                } else {
                    socket_.async_read_some(asio::buffer(buffer_), std::move(completion_value));
                }
            },
            "response read initiation failed");
    }

    void parse() {
        for (;;) {
            auto result =
                response_parser_->parse(std::string_view(response_bytes_).substr(response_offset_));
            if (result.need_more()) {
                if (response_bytes_.size() >= 65536) {
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
            response_offset_ += parsed->consumed_bytes();
            if (parsed->plan().informational()) {
                continue;
            }
            const auto status = static_cast<std::uint32_t>(parsed->head().status().value());
            complete(status == config_.expected_status_,
                status == config_.expected_status_
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
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_);
        target_.record_probe(
            healthy,
            healthy ? config_.healthy_threshold_ : config_.unhealthy_threshold_,
            static_cast<std::uint32_t>((std::min)(elapsed.count(), std::int64_t{600000})), error);
        notify_retired_if_ready();
    }

    void log(std::string_view message) const noexcept {
        try {
            std::cerr << "flexedge node health probe " << config_.host_ << ':' << config_.port_
                      << ": " << message << '\n';
        } catch (...) {
        }
    }

    void log(std::string_view message, const std::exception& error) const noexcept {
        try {
            std::cerr << "flexedge node health probe " << config_.host_ << ':' << config_.port_
                      << ": " << message << ": " << error.what() << '\n';
        } catch (...) {
        }
    }

    ruvia::event_loop loop_;
    origin_health_registry::target target_;
    origin_tls_context& tls_context_;
    origin_probe_config config_;
    std::function<void()> on_retired_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    std::optional<origin_tls_context::stream> tls_stream_;
    asio::steady_timer timer_;
    std::string request_bytes_;
    std::unique_ptr<ruvia::http1_client_response_parser> response_parser_;
    std::array<char, 4096> buffer_{};
    std::string response_bytes_;
    std::size_t response_offset_{};
    std::chrono::steady_clock::time_point started_at_{std::chrono::steady_clock::now()};
    std::size_t pending_operations_{};
    bool completed_{};
    bool retired_notified_{};
};

}  // namespace flexedge::node
