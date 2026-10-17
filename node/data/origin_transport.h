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

#include "node/data/origin_http2_connection.h"
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
    ~origin_transport() {
        close();
    }

    void connect(std::string host, std::uint16_t port, bool secure, completion completion_value, bool allow_http2 = true, bool require_http2 = false) {
        reset();
        allow_http2_ = allow_http2;
        require_http2_ = require_http2;
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
        if (http2_) {
            const asio::const_buffer bytes(buffer);
            http2_->request(std::string_view(static_cast<const char*>(bytes.data()), bytes.size()), std::forward<handler_type>(handler));
        } else if (tls_stream_) {
            asio::async_write(*tls_stream_, buffer, std::forward<handler_type>(handler));
        } else {
            asio::async_write(socket_, buffer, std::forward<handler_type>(handler));
        }
    }

    template <typename mutable_buffer_type, typename handler_type>
    void read(const mutable_buffer_type& buffer, handler_type&& handler) {
        if (http2_) {
            http2_->read(asio::mutable_buffer(buffer), std::forward<handler_type>(handler));
        } else if (tls_stream_) {
            tls_stream_->async_read_some(buffer, std::forward<handler_type>(handler));
        } else {
            socket_.async_read_some(buffer, std::forward<handler_type>(handler));
        }
    }

    void reset() noexcept {
        ++generation_;
        if (http2_) {
            http2_->close();
            http2_.reset();
        }
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
        if (http2_) {
            http2_->close();
        }
        resolver_.cancel();
        std::error_code ignored;
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_ && reusable_ && (!http2_ || http2_->reusable());
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
            tls_stream_.emplace(tls_context_.get_stream(std::move(socket_), connect_host_, allow_http2_));
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
            if (require_http2_) {
                http2_ = std::make_shared<origin_http2_connection>(std::move(socket_), false);
                http2_->start();
            }
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
        const unsigned char* protocol = nullptr;
        unsigned int size{};
        SSL_get0_alpn_selected(tls_stream_->native_handle(), &protocol, &size);
        if (require_http2_ && !(size == 2 && std::string_view(reinterpret_cast<const char*>(protocol), size) == "h2")) {
            complete_connect(generation, std::make_error_code(std::errc::protocol_not_supported));
            return;
        }
        if (size == 2 && std::string_view(reinterpret_cast<const char*>(protocol), size) == "h2") {
            http2_ = std::make_shared<origin_http2_connection>(std::move(*tls_stream_), true);
            tls_stream_.reset();
            http2_->start();
        }
        connected_ = true;
        complete_connect(generation, {});
    }

    asio::any_io_executor executor_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    origin_tls_context& tls_context_;
    std::optional<origin_tls_context::stream> tls_stream_;
    std::shared_ptr<origin_http2_connection> http2_{};
    bool allow_http2_{true};
    bool require_http2_{false};
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
