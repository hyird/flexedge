#pragma once

#include <functional>
#include <cstdint>
#include <exception>
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

class OriginTransport final {
  public:
    using Completion = std::function<void(const std::error_code&, HttpWireProtocol)>;

    OriginTransport(asio::any_io_executor executor, OriginTlsContext& tlsContext)
        : executor_(std::move(executor)), resolver_(executor_), socket_(executor_),
          tlsContext_(tlsContext) {}

    void connect(std::string host, std::uint16_t port, bool secure, bool http2Enabled,
                 Completion completion) {
        reset();
        const auto generation = generation_;
        auto sharedCompletion = std::make_shared<Completion>(std::move(completion));
        auto sharedHost = std::make_shared<std::string>(std::move(host));
        resolver_.async_resolve(
            *sharedHost, std::to_string(port),
            [this, sharedHost, secure, http2Enabled, generation,
             sharedCompletion](const std::error_code& error,
                               const asio::ip::tcp::resolver::results_type& endpoints) mutable {
                handleResolved(std::move(sharedHost), secure, http2Enabled, generation,
                               std::move(sharedCompletion), error, endpoints);
            });
    }

    template <typename ConstBuffer, typename Handler>
    void write(const ConstBuffer& buffer, Handler&& handler) {
        if (tlsStream_) {
            asio::async_write(*tlsStream_, buffer, std::forward<Handler>(handler));
        } else {
            asio::async_write(socket_, buffer, std::forward<Handler>(handler));
        }
    }

    template <typename MutableBuffer, typename Handler>
    void read(const MutableBuffer& buffer, Handler&& handler) {
        if (tlsStream_) {
            tlsStream_->async_read_some(buffer, std::forward<Handler>(handler));
        } else {
            socket_.async_read_some(buffer, std::forward<Handler>(handler));
        }
    }

    void reset() noexcept {
        ++generation_;
        connected_ = false;
        resolver_.cancel();
        std::error_code ignored;
        if (tlsStream_) {
            ignored = tlsStream_->lowest_layer().close(ignored);
            tlsStream_.reset();
        }
        ignored = socket_.close(ignored);
        socket_ = asio::ip::tcp::socket(executor_);
    }

    void close() noexcept {
        connected_ = false;
        resolver_.cancel();
        std::error_code ignored;
        if (tlsStream_) {
            ignored = tlsStream_->lowest_layer().close(ignored);
        }
        ignored = socket_.close(ignored);
    }

    [[nodiscard]] bool connected() const noexcept { return connected_; }

  private:
    using SharedCompletion = std::shared_ptr<Completion>;

    void handleResolved(std::shared_ptr<std::string> host, bool secure, bool http2Enabled,
                        std::uint64_t generation, SharedCompletion completion,
                        const std::error_code& error,
                        const asio::ip::tcp::resolver::results_type& endpoints) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted, HttpWireProtocol::kHttp1);
            return;
        }
        if (error) {
            (*completion)(error, HttpWireProtocol::kHttp1);
            return;
        }
        asio::async_connect(
            socket_, endpoints,
            [this, host = std::move(host), secure, http2Enabled, generation,
             completion = std::move(completion)](const std::error_code& connectError,
                                                 const asio::ip::tcp::endpoint&) mutable {
                handleConnected(host, secure, http2Enabled, generation, std::move(completion),
                                connectError);
            });
    }

    OriginTlsContext::Stream* prepareTls(const std::string& host, bool http2Enabled,
                                         const SharedCompletion& completion) {
        try {
            tlsStream_.emplace(tlsContext_.stream(std::move(socket_), host, http2Enabled));
        } catch (const std::exception& error) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << host << ": " << error.what() << '\n';
            (*completion)(std::make_error_code(std::errc::protocol_error),
                          HttpWireProtocol::kHttp1);
            return nullptr;
        } catch (...) {
            std::cerr << "flexedge node origin transport: TLS stream preparation failed for "
                      << host << " with unknown exception\n";
            (*completion)(std::make_error_code(std::errc::protocol_error),
                          HttpWireProtocol::kHttp1);
            return nullptr;
        }
        return &*tlsStream_;
    }

    void handleConnected(const std::shared_ptr<std::string>& host, bool secure, bool http2Enabled,
                         std::uint64_t generation, SharedCompletion completion,
                         const std::error_code& error) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted, HttpWireProtocol::kHttp1);
            return;
        }
        if (error) {
            (*completion)(error, HttpWireProtocol::kHttp1);
            return;
        }
        if (!secure) {
            connected_ = true;
            (*completion)({}, HttpWireProtocol::kHttp1);
            return;
        }
        auto* stream = prepareTls(*host, http2Enabled, completion);
        if (stream == nullptr) {
            return;
        }
        stream->async_handshake(asio::ssl::stream_base::client,
                                [this, generation, completion = std::move(completion)](
                                    const std::error_code& handshakeError) mutable {
                                    handleHandshake(generation, completion, handshakeError);
                                });
    }

    void handleHandshake(std::uint64_t generation, const SharedCompletion& completion,
                         const std::error_code& error) {
        if (generation != generation_) {
            (*completion)(asio::error::operation_aborted, HttpWireProtocol::kHttp1);
            return;
        }
        if (error) {
            connected_ = false;
            (*completion)(error, HttpWireProtocol::kHttp1);
            return;
        }
        if (!tlsStream_) {
            connected_ = false;
            (*completion)(std::make_error_code(std::errc::protocol_error),
                          HttpWireProtocol::kHttp1);
            return;
        }
        connected_ = true;
        (*completion)({}, negotiatedHttpProtocol(tlsStream_->native_handle()));
    }

    asio::any_io_executor executor_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    OriginTlsContext& tlsContext_;
    std::optional<OriginTlsContext::Stream> tlsStream_;
    std::uint64_t generation_{};
    bool connected_{};
};

} // namespace flexedge::node
