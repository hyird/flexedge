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
        auto shared_completion = std::make_shared<completion>(std::move(completion_value));
        auto shared_host = std::make_shared<std::string>(std::move(host));
        resolver_.async_resolve(
            *shared_host, std::to_string(port),
            [this, shared_host, secure, generation,
                shared_completion](const std::error_code& error,
                const asio::ip::tcp::resolver::results_type& endpoints) mutable {
                handle_resolved(std::move(shared_host), secure, generation,
                    std::move(shared_completion), error, endpoints);
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
        resolver_.cancel();
        std::error_code ignored;
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
            tls_stream_.reset();
        }
        ignored = socket_.close(ignored);
        socket_ = asio::ip::tcp::socket(executor_);
    }

    void close() noexcept {
        connected_ = false;
        resolver_.cancel();
        std::error_code ignored;
        if (tls_stream_) {
            ignored = tls_stream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
    }

    [[nodiscard]] bool connected() const noexcept {
        return connected_;
    }

private:
    using shared_completion = std::shared_ptr<completion>;

    void handle_resolved(std::shared_ptr<std::string> host, bool secure,
        std::uint64_t generation, shared_completion completion,
        const std::error_code& error,
        const asio::ip::tcp::resolver::results_type& endpoints) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted);
            return;
        }
        if (error) {
            (*completion)(error);
            return;
        }
        asio::async_connect(
            socket_, endpoints,
            [this, host = std::move(host), secure, generation,
                completion = std::move(completion)](const std::error_code& connect_error,
                const asio::ip::tcp::endpoint&) mutable {
                handle_connected(host, secure, generation, std::move(completion), connect_error);
            });
    }

    origin_tls_context::stream* prepare_tls(const std::string& host,
        const shared_completion& completion) {
        try {
            tls_stream_.emplace(tls_context_.get_stream(std::move(socket_), host));
        } catch (const std::exception& error) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << host << ": " << error.what() << '\n';
            (*completion)(std::make_error_code(std::errc::protocol_error));
            return nullptr;
        } catch (...) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << host << " with unknown exception\n";
            (*completion)(std::make_error_code(std::errc::protocol_error));
            return nullptr;
        }
        return &*tls_stream_;
    }

    void handle_connected(const std::shared_ptr<std::string>& host, bool secure,
        std::uint64_t generation, shared_completion completion,
        const std::error_code& error) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted);
            return;
        }
        if (error) {
            (*completion)(error);
            return;
        }
        if (!secure) {
            connected_ = true;
            (*completion)({});
            return;
        }
        auto* stream = prepare_tls(*host, completion);
        if (stream == nullptr) {
            return;
        }
        stream->async_handshake(asio::ssl::stream_base::client,
            [this, generation, completion = std::move(completion)](
                const std::error_code& handshake_error) mutable {
                handle_handshake(generation, completion, handshake_error);
            });
    }

    void handle_handshake(std::uint64_t generation, const shared_completion& completion,
        const std::error_code& error) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted);
            return;
        }
        if (error) {
            connected_ = false;
            (*completion)(error);
            return;
        }
        if (!tls_stream_) {
            connected_ = false;
            (*completion)(std::make_error_code(std::errc::protocol_error));
            return;
        }
        connected_ = true;
        (*completion)({});
    }

    asio::any_io_executor executor_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    origin_tls_context& tls_context_;
    std::optional<origin_tls_context::stream> tls_stream_;
    std::uint64_t generation_{};
    bool connected_{};
};

}  // namespace flexedge::node
