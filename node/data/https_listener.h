#pragma once

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <asio/bind_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>

#if defined(SO_REUSEPORT) && !defined(_WIN32)
#include <sys/socket.h>
#endif

#include <ruvia/core/EventLoop.h>

#include "node/data/http_listener.h"
#include "node/data/http2_session.h"
#include "node/data/origin_health.h"
#include "node/data/tls_context.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class TlsSession final : public std::enable_shared_from_this<TlsSession> {
  public:
    using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

    TlsSession(asio::ip::tcp::socket socket, RuntimeState& runtime, OriginHealthRegistry& health,
               std::shared_ptr<const TlsContextSet> contexts, RuntimeMetrics& metrics,
               NodeLogBuffer::Producer& logs, OriginConnectionPool& originConnections,
               BufferedBytesBudget& requestBuffers, BufferedBytesBudget& responseBuffers,
               std::uint64_t sequence)
        : stream_(std::move(socket), contexts->defaultContext()), runtime_(runtime),
          health_(health), contexts_(std::move(contexts)), metrics_(metrics), logs_(logs),
          originConnections_(originConnections), requestBuffers_(requestBuffers),
          responseBuffers_(responseBuffers), timer_(stream_.get_executor()), sequence_(sequence) {
        captureTlsClientFingerprint(stream_.native_handle(), tlsFingerprint_);
    }

    void start() {
        const auto self = shared_from_this();
        timer_.expires_after(std::chrono::seconds(10));
        timer_.async_wait([self](const std::error_code& error) {
            if (!error) {
                self->close();
            }
        });
        stream_.async_handshake(asio::ssl::stream_base::server, [self](
                                                                    const std::error_code& error) {
            std::error_code ignored;
            self->timer_.cancel(ignored);
            if (error) {
                self->close();
                return;
            }
            auto tlsFingerprint = std::move(self->tlsFingerprint_);
            releaseTlsClientFingerprint(self->stream_.native_handle());
            if (negotiatedHttpProtocol(self->stream_.native_handle()) == HttpWireProtocol::kHttp2) {
                std::make_shared<Http2Session>(std::move(self->stream_), self->runtime_,
                                               self->health_, self->metrics_, self->logs_,
                                               self->originConnections_, self->requestBuffers_,
                                               self->responseBuffers_, self->sequence_,
                                               self->contexts_, std::move(tlsFingerprint))
                    ->start();
            } else {
                std::make_shared<BasicHttpSession<Stream>>(
                    std::move(self->stream_), self->runtime_, self->health_, self->metrics_,
                    self->logs_, self->originConnections_, self->requestBuffers_,
                    self->responseBuffers_, self->sequence_, self->contexts_, true,
                    std::move(tlsFingerprint))
                    ->start();
            }
        });
    }

  private:
    void close() noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
        ignored = stream_.lowest_layer().close(ignored);
    }

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
};

class HttpsListener final : public std::enable_shared_from_this<HttpsListener> {
  public:
    HttpsListener(ruvia::EventLoop owner, RuntimeState& runtime, OriginHealthRegistry& health,
                  TlsContextRegistry& tlsContexts, RuntimeMetrics& metrics,
                  NodeLogBuffer::Producer& logs, OriginConnectionPool& originConnections,
                  BufferedBytesBudget& requestBuffers, BufferedBytesBudget& responseBuffers,
                  const asio::ip::tcp::endpoint& endpoint)
        : owner_(std::move(owner)), runtime_(runtime), health_(health), tlsContexts_(tlsContexts),
          metrics_(metrics), logs_(logs), originConnections_(originConnections),
          requestBuffers_(requestBuffers), responseBuffers_(responseBuffers),
          acceptor_(owner_.ioContext()), activationTimer_(owner_.ioContext()) {
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
        stopRegistration_ = owner_.onStop([this] { stop(); });
    }

    ~HttpsListener() { stop(); }

    void requestStart(std::shared_ptr<const ListenerActivationGate> activationGate = nullptr) {
        activationGate_ = std::move(activationGate);
        const auto self = shared_from_this();
        if (!owner_.post([self] { self->start(); }).accepted()) {
            throw std::runtime_error("edge HTTPS listener worker is stopping");
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint localEndpoint() const {
        return acceptor_.local_endpoint();
    }

    void requestStop() noexcept {
        if (owner_.isCurrent()) {
            stop();
            return;
        }
        const auto self = shared_from_this();
        if (!owner_.post([self] { self->stop(); }).accepted()) {
            stop();
        }
    }

    void stop() noexcept {
        std::error_code ignored;
        activationTimer_.cancel(ignored);
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
    }

  private:
    void start() {
        if (activationGate_ && !activationGate_->active()) {
            activationTimer_.expires_after(std::chrono::milliseconds(1));
            const auto self = shared_from_this();
            activationTimer_.async_wait([self](const std::error_code& error) {
                if (!error) {
                    self->start();
                }
            });
            return;
        }
        accept();
    }

    void accept() {
        if (!acceptor_.is_open()) {
            return;
        }
        auto socket = std::make_shared<asio::ip::tcp::socket>(owner_.ioContext());
        const auto self = shared_from_this();
        acceptor_.async_accept(
            *socket,
            asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                if (!error) {
                    const auto contexts = self->tlsContexts_.current();
                    if (contexts && !contexts->empty()) {
                        std::make_shared<TlsSession>(
                            std::move(*socket), self->runtime_, self->health_, contexts,
                            self->metrics_, self->logs_, self->originConnections_,
                            self->requestBuffers_, self->responseBuffers_, ++self->sequence_)
                            ->start();
                    }
                }
                if (self->acceptor_.is_open()) {
                    self->accept();
                }
            }));
    }

    ruvia::EventLoop owner_;
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
    std::shared_ptr<const ListenerActivationGate> activationGate_;
    std::uint64_t sequence_{};
};

} // namespace flexedge::node
