#pragma once

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>

#if defined(SO_REUSEPORT) && !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <ruvia/core/EventLoop.h>
#include <ruvia/core/WorkerNotification.h>

#include "node/data/http_listener.h"
#include "node/data/http2_session.h"
#include "node/data/origin_health.h"
#include "node/data/tls_context.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class HttpsConnection final {
  public:
    explicit HttpsConnection(std::function<void()> onRetired)
        : onRetired_(std::move(onRetired)) {}
    ~HttpsConnection() { if (onRetired_) onRetired_(); }

    void setClose(std::function<void()> close) { close_ = std::move(close); }
    void close() noexcept { if (close_) close_(); }

  private:
    std::function<void()> close_;
    std::function<void()> onRetired_;
};

class TlsSession final : public std::enable_shared_from_this<TlsSession> {
  public:
    using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

    TlsSession(asio::ip::tcp::socket socket, RuntimeState& runtime, OriginHealthRegistry& health,
               std::shared_ptr<const TlsContextSet> contexts, RuntimeMetrics& metrics,
               NodeLogBuffer::Producer& logs, OriginConnectionPool& originConnections,
               BufferedBytesBudget& requestBuffers, BufferedBytesBudget& responseBuffers,
               std::uint64_t sequence, std::shared_ptr<HttpsConnection> connection)
        : stream_(std::move(socket), contexts->defaultContext()), runtime_(runtime),
          health_(health), contexts_(std::move(contexts)), metrics_(metrics), logs_(logs),
          originConnections_(originConnections), requestBuffers_(requestBuffers),
          responseBuffers_(responseBuffers), timer_(stream_.get_executor()), sequence_(sequence),
          connection_(std::move(connection)) {
        captureTlsClientFingerprint(stream_.native_handle(), tlsFingerprint_);
    }

    void start() {
        const auto self = shared_from_this();
        connection_->setClose([weak = weak_from_this()] {
            if (const auto session = weak.lock()) session->close();
        });
        try {
            timer_.expires_after(std::chrono::seconds(10));
            timer_.async_wait([self](const std::error_code& error) {
                if (!error) self->close();
            });
            stream_.async_handshake(asio::ssl::stream_base::server, [self](
                                                                        const std::error_code& error) {
                std::error_code ignored;
                self->timer_.cancel(ignored);
                if (error) {
                    self->close();
                    return;
                }
                try {
                    auto tlsFingerprint = std::move(self->tlsFingerprint_);
                    releaseTlsClientFingerprint(self->stream_.native_handle());
                    auto connection = self->connection_;
                    if (negotiatedHttpProtocol(self->stream_.native_handle()) == HttpWireProtocol::kHttp2) {
                    auto session = std::make_shared<Http2Session>(
                        std::move(self->stream_), self->runtime_, self->health_, self->metrics_,
                        self->logs_, self->originConnections_, self->requestBuffers_,
                        self->responseBuffers_, self->sequence_, self->contexts_,
                        std::move(tlsFingerprint), [connection] {});
                    connection->setClose([weak = std::weak_ptr<Http2Session>(session)] {
                        if (const auto active = weak.lock()) active->close();
                    });
                    try { session->start(); } catch (...) { session->close(); }
                } else {
                    auto session = std::make_shared<BasicHttpSession<Stream>>(
                        std::move(self->stream_), self->runtime_, self->health_, self->metrics_,
                        self->logs_, self->originConnections_, self->requestBuffers_,
                        self->responseBuffers_, self->sequence_, self->contexts_, true,
                        std::move(tlsFingerprint), [connection] {});
                    connection->setClose([weak = std::weak_ptr<BasicHttpSession<Stream>>(session)] {
                        if (const auto active = weak.lock()) active->close();
                    });
                    try { session->start(); } catch (...) { session->close(); }
                    }
                } catch (...) {
                    self->close();
                }
            });
        } catch (...) {
            close();
            throw;
        }
    }

  public:
    void close() noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
        ignored = stream_.lowest_layer().close(ignored);
    }

  private:
    Stream stream_;
    RuntimeState& runtime_;
    OriginHealthRegistry& health_;
    std::shared_ptr<const TlsContextSet> contexts_;
    RuntimeMetrics& metrics_;
    NodeLogBuffer::Producer& logs_;
    OriginConnectionPool& originConnections_;
    BufferedBytesBudget& requestBuffers_;
    BufferedBytesBudget& responseBuffers_;
    asio::steady_timer timer_;
    std::uint64_t sequence_{};
    std::string tlsFingerprint_;
    std::shared_ptr<HttpsConnection> connection_;
};

class HttpsListener final : public std::enable_shared_from_this<HttpsListener> {
  public:
    template <typename... Args>
    [[nodiscard]] static std::shared_ptr<HttpsListener> create(Args&&... args) {
        auto listener = std::shared_ptr<HttpsListener>(new HttpsListener(std::forward<Args>(args)...));
        listener->stopRegistration_ = listener->owner_.onStop(
            [weak = std::weak_ptr<HttpsListener>(listener)]() -> ruvia::Task<void> {
                if (const auto self = weak.lock()) co_await self->stopAndWait();
                co_return;
            });
        return listener;
    }

  private:
    HttpsListener(ruvia::EventLoop owner, RuntimeState& runtime, OriginHealthRegistry& health,
                  TlsContextRegistry& tlsContexts, RuntimeMetrics& metrics,
                  NodeLogBuffer::Producer& logs, OriginConnectionPool& originConnections,
                  BufferedBytesBudget& requestBuffers, BufferedBytesBudget& responseBuffers,
                  const asio::ip::tcp::endpoint& endpoint)
        : owner_(std::move(owner)), runtime_(runtime), health_(health), tlsContexts_(tlsContexts),
          metrics_(metrics), logs_(logs), originConnections_(originConnections),
          requestBuffers_(requestBuffers), responseBuffers_(responseBuffers),
          acceptor_(owner_.ioContext()), activationTimer_(owner_.ioContext()),
          stopNotification_(owner_) {
        std::error_code error;
        error = acceptor_.open(endpoint.protocol(), error);
        if (!error) {
            error = acceptor_.set_option(asio::socket_base::reuse_address(true), error);
        }
#if defined(SO_REUSEPORT) && !defined(_WIN32)
        if (!error) {
            int enabled = 1;
            if (::setsockopt(acceptor_.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
                             sizeof(enabled)) != 0) {
                throw std::system_error(errno, std::generic_category(),
                                        "could not enable HTTPS SO_REUSEPORT");
            }
        }
#endif
        if (!error) {
            error = acceptor_.bind(endpoint, error);
        }
        if (!error) {
            error = acceptor_.listen(asio::socket_base::max_listen_connections, error);
        }
        if (error) {
            throw std::system_error(error, "could not start edge HTTPS listener");
        }

    }

  public:
    ~HttpsListener() {
        std::lock_guard lock(lifecycleMutex_);
        if (loopStopComplete_ || startRequested_) return;
        stopping_ = true;
        std::error_code ignored;
        ignored = acceptor_.close(ignored);
    }

    void requestStart(std::shared_ptr<const ListenerActivationGate> activationGate = nullptr) {
        std::lock_guard lock(lifecycleMutex_);
        if (loopStopComplete_) throw std::runtime_error("edge HTTPS listener worker is stopping");
        startRequested_ = true;
        const auto self = shared_from_this();
        try {
            if (!owner_.post([self, gate = std::move(activationGate)] {
                    if (self->started_) return;
                    self->started_ = true;
                    self->activationGate_ = gate;
                    self->start();
                }).accepted()) {
                throw std::runtime_error("edge HTTPS listener worker is stopping");
            }
        } catch (...) {
            startRequested_ = false;
            throw;
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const {
        return acceptor_.local_endpoint();
    }

    void requestStop() noexcept {
        std::lock_guard lock(lifecycleMutex_);
        if (loopStopComplete_) return;
        if (!startRequested_) {
            stopping_ = true;
            std::error_code ignored;
            ignored = acceptor_.close(ignored);
            loopStopComplete_ = true;
            return;
        }
        if (owner_.isCurrent()) {
            stop();
            return;
        }
        asio::post(owner_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) self->stop();
        });
    }

  private:
    // Socket cancellation and accept initiation must stay on the owning loop.
    ruvia::Task<void> stopAndWait() {
        {
            std::lock_guard lock(lifecycleMutex_);
            stop();
        }
        while (pendingOperations_ != 0 || activeConnections_ != 0)
            co_await stopNotification_.wait();
        stopNotification_.close();
        std::lock_guard lock(lifecycleMutex_);
        loopStopComplete_ = true;
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        activationTimer_.cancel(ignored);
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
        for (const auto& weak : connections_) {
            if (const auto connection = weak.lock()) connection->close();
        }
    }

  private:
    void start() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        if (activationGate_ && !activationGate_->active()) {
            const auto self = shared_from_this();
            bool operationStarted = false;
            try {
                activationTimer_.expires_after(std::chrono::milliseconds(1));
                ++pendingOperations_;
                operationStarted = true;
                activationTimer_.async_wait([self](const std::error_code& error) {
                    self->operationCompleted();
                    if (!error) self->start();
                });
            } catch (...) {
                if (operationStarted) operationCompleted();
                stop();
                return;
            }
            return;
        }
        accept();
    }

    void operationCompleted() noexcept {
        --pendingOperations_;
        static_cast<void>(stopNotification_.notify());
    }

    void connectionRetired() noexcept {
        --activeConnections_;
        static_cast<void>(stopNotification_.notify());
    }

    void accept() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::shared_ptr<HttpsListener> self;
        try {
            socket = std::make_shared<asio::ip::tcp::socket>(owner_.ioContext());
            self = shared_from_this();
        } catch (...) {
            stop();
            return;
        }
        ++pendingOperations_;
        try {
            acceptor_.async_accept(
                *socket,
                asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                    self->operationCompleted();
                    if (!error && !self->stopping_) {
                        const auto contexts = self->tlsContexts_.current();
                        if (contexts && !contexts->empty()) {
                            std::shared_ptr<HttpsConnection> connection;
                            try {
                                connection = std::make_shared<HttpsConnection>(
                                    [weak = self->weak_from_this()] {
                                        if (const auto listener = weak.lock())
                                            listener->connectionRetired();
                                    });
                                ++self->activeConnections_;
                                std::erase_if(self->connections_, [](const auto& active) {
                                    return active.expired();
                                });
                                self->connections_.push_back(connection);
                                auto session = std::make_shared<TlsSession>(
                                    std::move(*socket), self->runtime_, self->health_, contexts,
                                    self->metrics_, self->logs_, self->originConnections_,
                                    self->requestBuffers_, self->responseBuffers_,
                                    ++self->sequence_, connection);
                                session->start();
                            } catch (...) {
                                if (connection) connection->close();
                                connection.reset();
                            }
                        }
                    }
                    if (self->acceptor_.is_open()) self->accept();
                }));
        } catch (...) {
            operationCompleted();
            stop();
        }
    }

    ruvia::EventLoop owner_;
    std::mutex lifecycleMutex_;
    bool startRequested_{};
    bool loopStopComplete_{};
    RuntimeState& runtime_;
    OriginHealthRegistry& health_;
    TlsContextRegistry& tlsContexts_;
    RuntimeMetrics& metrics_;
    NodeLogBuffer::Producer& logs_;
    OriginConnectionPool& originConnections_;
    BufferedBytesBudget& requestBuffers_;
    BufferedBytesBudget& responseBuffers_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer activationTimer_;
    ruvia::EventLoopStopRegistration stopRegistration_;
    ruvia::WorkerNotification stopNotification_;
    std::size_t pendingOperations_{};
    std::size_t activeConnections_{};
    std::vector<std::weak_ptr<HttpsConnection>> connections_;
    bool stopping_{};
    std::shared_ptr<const ListenerActivationGate> activationGate_;
    std::uint64_t sequence_{};
    bool started_{}; // Accessed only by the owning event loop.
};

} // namespace flexedge::node
