#pragma once

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

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include "node/data/origin_tls.h"
#include "node/runtime/dns_resolution.h"

namespace flexedge::node {

class origin_transport final {
public:
    using completion = std::function<void(const std::error_code&)>;

    origin_transport(asio::any_io_executor executor, origin_tls_context& tls_context)
        : executor_(std::move(executor)),
          resolver_(executor_),
          socket_(executor_),
          tls_context_(tls_context) {}

    void connect(std::string host, std::uint16_t port, bool secure, completion completion_value) {
        reset();
        const auto generation = generation_;
        connect_generation_ = generation;
        connect_host_ = std::move(host);
        connect_completion_ = std::move(completion_value);
        resolver_.async_resolve(
            connect_host_, std::to_string(port),
            [this, secure, generation](const std::error_code& error,
                const asio::ip::tcp::resolver::results_type& endpoints) {
                handle_resolved(secure, generation, error, endpoints);
            });
    }

    template <typename const_buffer_type, typename handler_type>
    void write(const const_buffer_type& buffer, handler_type&& handler) {
        if (tls_stream_) {
            asio::async_write(*tls_stream_, buffer, std::forward<handler_type>(handler));
        } else {
            asio::async_write(socket_, buffer, std::forward<handler_type>(handler));
        }
    }

    template <typename mutable_buffer_type, typename handler_type>
    void read(const mutable_buffer_type& buffer, handler_type&& handler) {
        if (tls_stream_) {
            tls_stream_->async_read_some(buffer, std::forward<handler_type>(handler));
        } else {
            socket_.async_read_some(buffer, std::forward<handler_type>(handler));
        }
    }

    void reset() noexcept {
        ++generation_;
        connected_ = false;
        reusable_ = true;
        tunnel_ = false;
        addresses_.clear();
        resolver_.cancel();
        std::error_code ignored;
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
            tls_stream_.reset();
        }
        ignored = socket_.close(ignored);
        socket_ = asio::ip::tcp::socket(executor_);
        if (auto pending = std::move(connect_completion_)) {
            try {
                pending(asio::error::operation_aborted);
            } catch (...) {
            }
        }
    }

    void close() noexcept {
        // A DNS/connect completion already queued before cancellation must not reopen the
        // socket or start a TLS handshake after its owner has stopped the exchange/probe.
        ++generation_;
        connected_ = false;
        resolver_.cancel();
        std::error_code ignored;
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_ && reusable_;
    }

    [[nodiscard]] const dns_addresses& addresses() const noexcept {
        return addresses_;
    }
    void invalidate_dns() noexcept {
        reusable_ = false;
        if (tunnel_) {
            close();
        }
    }
    void enter_tunnel() noexcept {
        tunnel_ = true;
        if (!reusable_) {
            close();
        }
    }

private:
    void complete_connect(std::uint64_t generation, const std::error_code& error) {
        if (generation != connect_generation_) {
            return;
        }
        if (auto callback = std::move(connect_completion_)) {
            callback(error);
        }
    }

    void handle_resolved(bool secure, std::uint64_t generation,
        const std::error_code& error,
        const asio::ip::tcp::resolver::results_type& endpoints) {
        if (generation != generation_) {
            complete_connect(generation, asio::error::operation_aborted);
            return;
        }
        if (error) {
            complete_connect(generation, error);
            return;
        }
        try {
            addresses_ = resolved_addresses(endpoints);
        } catch (...) {
            complete_connect(generation, std::make_error_code(std::errc::not_enough_memory));
            return;
        }
        asio::async_connect(
            socket_, endpoints,
            [this, secure, generation](const std::error_code& connect_error,
                const asio::ip::tcp::endpoint&) {
                handle_connected(secure, generation, connect_error);
            });
    }

    origin_tls_context::stream* prepare_tls(std::uint64_t generation) {
        try {
            tls_stream_.emplace(tls_context_.get_stream(std::move(socket_), connect_host_));
        } catch (const std::exception& error) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << connect_host_ << ": " << error.what() << '\n';
            complete_connect(generation, std::make_error_code(std::errc::protocol_error));
            return nullptr;
        } catch (...) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << connect_host_ << " with unknown exception\n";
            complete_connect(generation, std::make_error_code(std::errc::protocol_error));
            return nullptr;
        }
        return &*tls_stream_;
    }

    void handle_connected(bool secure, std::uint64_t generation,
        const std::error_code& error) {
        if (generation != generation_) {
            complete_connect(generation, asio::error::operation_aborted);
            return;
        }
        if (error) {
            complete_connect(generation, error);
            return;
        }
        if (!secure) {
            connected_ = true;
            complete_connect(generation, {});
            return;
        }
        auto* stream = prepare_tls(generation);
        if (stream == nullptr) {
            return;
        }
        stream->async_handshake(asio::ssl::stream_base::client,
            [this, generation](const std::error_code& handshake_error) {
                handle_handshake(generation, handshake_error);
            });
    }

    void handle_handshake(std::uint64_t generation, const std::error_code& error) {
        if (generation != generation_) {
            complete_connect(generation, asio::error::operation_aborted);
            return;
        }
        if (error) {
            connected_ = false;
            complete_connect(generation, error);
            return;
        }
        if (!tls_stream_) {
            connected_ = false;
            complete_connect(generation, std::make_error_code(std::errc::protocol_error));
            return;
        }
        connected_ = true;
        complete_connect(generation, {});
    }

    asio::any_io_executor executor_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    origin_tls_context& tls_context_;
    std::optional<origin_tls_context::stream> tls_stream_;
    std::string connect_host_;
    completion connect_completion_;
    std::uint64_t connect_generation_{};
    std::uint64_t generation_{};
    bool connected_{};
    bool reusable_{true};
    bool tunnel_{};
    dns_addresses addresses_{};
};

}  // namespace flexedge::node
