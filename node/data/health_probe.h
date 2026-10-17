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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/ssl/error.hpp>
#include <asio/steady_timer.hpp>

#include "node/data/origin_health.h"
#include "node/data/origin_tls.h"
#include "node/data/origin_transport.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

struct origin_probe_config final {
    std::string protocol_;
    std::string http_version_{"auto"};
    std::string host_;
    std::uint16_t port_{};
    std::string path_{"/"};
    std::chrono::seconds timeout_{3};
    std::uint32_t expected_status_{200};
    std::uint32_t healthy_threshold_{2};
    std::uint32_t unhealthy_threshold_{3};

    bool operator==(const origin_probe_config&) const = default;
};

[[nodiscard]] inline origin_probe_config make_origin_probe_config(const v2::Website& website, const v2::Origin& origin) {
    return {.protocol_ = origin.protocol(), .http_version_ = website.origin_http_version(), .host_ = origin.host(), .port_ = static_cast<std::uint16_t>(origin.port()), .path_ = website.health_check_path(), .timeout_ = std::chrono::seconds(website.health_check_timeout_seconds()), .expected_status_ = website.health_check_expected_status() == 0 ? 200 : website.health_check_expected_status(), .healthy_threshold_ = website.healthy_threshold(), .unhealthy_threshold_ = website.unhealthy_threshold()};
}

// Sends one HEAD request to an origin over its own origin_transport and records whether the
// final status matched. Retires once completed and every started operation has finished.
class origin_health_probe final : public std::enable_shared_from_this<origin_health_probe> {
public:
    origin_health_probe(ruvia::event_loop loop, origin_probe_target target,
        origin_tls_context& tls_context, origin_probe_config config,
        std::function<void()> on_retired = {})
        : loop_(std::move(loop)),
          target_(std::move(target)),
          config_(std::move(config)),
          on_retired_(std::move(on_retired)),
          transport_(loop_.executor(), tls_context),
          timer_(loop_.io_context()) {}

    void set_on_retired(std::function<void()> callback) {
        on_retired_ = std::move(callback);
    }

    void cancel() noexcept {
        completed_ = true;
        std::error_code ignored;
        timer_.cancel(ignored);
        transport_.close();
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
            initiate_operation(
                [&] {
                    timer_.async_wait([self](const std::error_code& error) {
                        completion completion{self};
                        if (!error) {
                            self->complete(false, "timeout");
                        }
                    });
                },
                "async probe startup failed");
            if (completed_) {
                return;
            }
            initiate_operation(
                [&] {
                    transport_.connect(config_.host_, config_.port_, config_.protocol_ == "https",
                        [self](const std::error_code& error) {
                            completion completion{self};
                            if (self->completed_) {
                                return;
                            }
                            if (error) {
                                self->complete(false, connect_failure(error));
                                return;
                            }
                            self->write();
                        },config_.http_version_!="http1",config_.http_version_=="http2");
                },
                "async probe startup failed");
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

    static std::string_view connect_failure(const std::error_code& error) noexcept {
        if (error.category() == asio::error::get_netdb_category() ||
            error.category() == asio::error::get_addrinfo_category()) {
            return "dns lookup failed";
        }
        if (error.category() == asio::error::get_ssl_category() ||
            error.category() == asio::ssl::error::get_stream_category()) {
            return "TLS handshake failed";
        }
        return "connection failed";
    }

    template <typename initiation_type>
    void initiate_operation(initiation_type&& initiation, std::string_view failure) noexcept {
        ++pending_operations_;
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

    void write() {
        const auto self = shared_from_this();
        initiate_operation(
            [&] {
                transport_.write(asio::buffer(request_bytes_),
                    [self](const std::error_code& error, std::size_t) {
                        completion operation{self};
                        if (self->completed_) {
                            return;
                        }
                        if (error) {
                            self->complete(false, "request write failed");
                            return;
                        }
                        self->read();
                    });
            },
            "request write initiation failed");
    }

    void read() {
        const auto self = shared_from_this();
        initiate_operation(
            [&] {
                transport_.read(asio::buffer(buffer_),
                    [self](const std::error_code& error, std::size_t size) {
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
                    });
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
        transport_.close();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_at_);
        try {
            target_.record_probe(healthy,
                healthy ? config_.healthy_threshold_ : config_.unhealthy_threshold_,
                static_cast<std::uint32_t>((std::min)(elapsed.count(), std::int64_t{600000})), error);
        } catch (...) {
        }
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
    origin_probe_target target_;
    origin_probe_config config_;
    std::function<void()> on_retired_;
    origin_transport transport_;
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
