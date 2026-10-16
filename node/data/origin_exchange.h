#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <asio/buffer.hpp>

#include "node/data/origin_connection_pool.h"
#include "node/data/origin_response_codec.h"

namespace flexedge::node {

// One request/response exchange with an origin, used for streamed, buffered and upgraded
// responses alike. It takes an idle pooled transport for the key or connects a new one. A
// pooled transport that fails before any request byte is written was closed by the origin
// while idle, so the request is sent once more on a fresh connection. Every completion
// holds the exchange, so the transport outlives the operations started on it. Timeouts and
// the meaning of a failure belong to the caller.
class origin_exchange final : public std::enable_shared_from_this<origin_exchange> {
public:
    origin_exchange(origin_connection_pool& pool, origin_connection_key key, std::string request,
        origin_response_codec response)
        : pool_(pool),
          key_(std::move(key)),
          request_(std::move(request)),
          response_(std::move(response)) {}

    origin_exchange(const origin_exchange&) = delete;
    origin_exchange& operator=(const origin_exchange&) = delete;

    // Completes with `handler(error)` once the whole request is written.
    template <typename handler_type>
    void send(handler_type handler) {
        transport_ = pool_.acquire(key_);
        pooled_ = transport_->connected();
        if (pooled_) {
            write_request(std::move(handler));
        } else {
            connect(std::move(handler));
        }
    }

    // Completes with `handler(error, bytes)`; `bytes` stays valid until the next read.
    template <typename handler_type>
    void read(handler_type handler) {
        transport_->read(asio::buffer(buffer_),
            [self = shared_from_this(), handler = std::move(handler)](
                const std::error_code& error, std::size_t size) mutable {
                handler(error, std::string_view(self->buffer_.data(), size));
            });
    }

    // Writes tunnelled client bytes; completes with `handler(error)`.
    template <typename handler_type>
    void write(asio::const_buffer bytes, handler_type handler) {
        transport_->write(bytes, [self = shared_from_this(), handler = std::move(handler)](
                                     const std::error_code& error, std::size_t) mutable {
            handler(error);
        });
    }

    [[nodiscard]] origin_response_codec& response() noexcept {
        return response_;
    }

    // Pools the transport when its response completed with a reusable framing; closes it
    // otherwise. Ends the exchange.
    void release() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        if (!transport_ || !response_.origin_reusable()) {
            close_transport();
            return;
        }
        try {
            pool_.release(std::move(key_), std::move(transport_));
        } catch (...) {
            // An idle connection that cannot be pooled is closed with its owner.
        }
    }

    // Ends the exchange and cancels its operations.
    void close() noexcept {
        closed_ = true;
        close_transport();
    }

private:
    template <typename handler_type>
    void connect(handler_type handler) {
        transport_->connect(key_.host_, key_.port_, key_.secure_,
            [self = shared_from_this(), handler = std::move(handler)](
                const std::error_code& error) mutable {
                if (error || self->closed_) {
                    handler(error ? error : std::make_error_code(std::errc::operation_canceled));
                    return;
                }
                self->write_request(std::move(handler));
            });
    }

    template <typename handler_type>
    void write_request(handler_type handler) {
        transport_->write(asio::buffer(request_),
            [self = shared_from_this(), handler = std::move(handler)](
                const std::error_code& error, std::size_t size) mutable {
                if (error && size == 0 && self->pooled_ && !self->closed_) {
                    self->pooled_ = false;
                    self->connect(std::move(handler));
                    return;
                }
                handler(error);
            });
    }

    void close_transport() noexcept {
        if (transport_) {
            transport_->close();
        }
    }

    origin_connection_pool& pool_;
    origin_connection_key key_;
    std::string request_;
    origin_response_codec response_;
    std::unique_ptr<origin_transport> transport_;
    std::array<char, 16384> buffer_{};
    bool pooled_{};
    bool closed_{};
};

}  // namespace flexedge::node
