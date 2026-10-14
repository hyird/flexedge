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

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_signal.h>

#include "node/data/http_listener.h"
#include "node/data/http_protocol.h"
#include "node/data/origin_health.h"
#include "node/data/tls_context.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/runtime_state.h"

namespace flexedge::node {

class https_connection final {
public:
    explicit https_connection(std::function<void()> on_retired)
        : on_retired_(std::move(on_retired)) {}
    ~https_connection() {
        if (on_retired_) {
            on_retired_();
        }
    }

    void set_close(std::function<void()> close) {
        close_ = std::move(close);
    }
    void close() noexcept {
        if (close_) {
            close_();
        }
    }

private:
    std::function<void()> close_;
    std::function<void()> on_retired_;
};

class tls_session final : public std::enable_shared_from_this<tls_session> {
public:
    using stream = asio::ssl::stream<asio::ip::tcp::socket>;

    tls_session(asio::ip::tcp::socket socket, runtime_state& runtime, origin_health_registry& health,
        std::shared_ptr<const tls_context_set> contexts, runtime_metrics& metrics,
        connected_clients& clients, node_log_buffer::producer& logs,
        origin_connection_pool& origin_connections, buffered_bytes_budget& request_buffers,
        buffered_bytes_budget& response_buffers, std::uint64_t sequence,
        std::shared_ptr<https_connection> connection)
        : stream_(std::move(socket), contexts->default_context()),
          runtime_(runtime),
          health_(health),
          contexts_(std::move(contexts)),
          metrics_(metrics),
          clients_(clients),
          logs_(logs),
          origin_connections_(origin_connections),
          request_buffers_(request_buffers),
          response_buffers_(response_buffers),
          timer_(stream_.get_executor()),
          sequence_(sequence),
          connection_(std::move(connection)) {
        capture_tls_client_fingerprint(stream_.native_handle(), tls_fingerprint_);
    }

    void start() {
        const auto self = shared_from_this();
        connection_->set_close([weak = weak_from_this()] {
            if (const auto session = weak.lock()) {
                session->close();
            }
        });
        try {
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
                try {
                    auto tls_fingerprint = std::move(self->tls_fingerprint_);
                    release_tls_client_fingerprint(self->stream_.native_handle());
                    auto connection = self->connection_;
                    auto session = std::make_shared<basic_http_session<stream>>(
                        std::move(self->stream_), self->runtime_, self->health_, self->metrics_,
                        self->clients_, self->logs_, self->origin_connections_,
                        self->request_buffers_, self->response_buffers_, self->sequence_,
                        self->contexts_, true, std::move(tls_fingerprint), [connection] {});
                    connection->set_close([weak = std::weak_ptr<basic_http_session<stream>>(session)] {
                        if (const auto active = weak.lock()) {
                            active->close();
                        }
                    });
                    try {
                        session->start();
                    } catch (...) {
                        session->close();
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
    stream stream_;
    runtime_state& runtime_;
    origin_health_registry& health_;
    std::shared_ptr<const tls_context_set> contexts_;
    runtime_metrics& metrics_;
    connected_clients& clients_;
    node_log_buffer::producer& logs_;
    origin_connection_pool& origin_connections_;
    buffered_bytes_budget& request_buffers_;
    buffered_bytes_budget& response_buffers_;
    asio::steady_timer timer_;
    std::uint64_t sequence_{};
    std::string tls_fingerprint_;
    std::shared_ptr<https_connection> connection_;
};

class https_listener final : public std::enable_shared_from_this<https_listener> {
public:
    template <typename... args_type>
    [[nodiscard]] static std::shared_ptr<https_listener> create(args_type&&... args) {
        auto listener = std::shared_ptr<https_listener>(new https_listener(std::forward<args_type>(args)...));
        listener->stop_registration_ = listener->owner_.on_stop(
            [weak = std::weak_ptr<https_listener>(listener)]() -> ruvia::task<void> {
                if (const auto self = weak.lock()) {
                    co_await self->stop_and_wait();
                }
                co_return;
            });
        return listener;
    }

private:
    https_listener(ruvia::event_loop owner, runtime_state& runtime, origin_health_registry& health,
        tls_context_registry& tls_contexts, runtime_metrics& metrics, connected_clients& clients,
        node_log_buffer::producer& logs, origin_connection_pool& origin_connections,
        buffered_bytes_budget& request_buffers, buffered_bytes_budget& response_buffers,
        const asio::ip::tcp::endpoint& endpoint)
        : owner_(std::move(owner)),
          owner_worker_(owner_.handle()),
          runtime_(runtime),
          health_(health),
          tls_contexts_(tls_contexts),
          metrics_(metrics),
          clients_(clients),
          logs_(logs),
          origin_connections_(origin_connections),
          request_buffers_(request_buffers),
          response_buffers_(response_buffers),
          acceptor_(owner_.io_context()),
          activation_timer_(owner_.io_context()),
          stop_signal_(owner_worker_) {
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
    ~https_listener() {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_ || start_requested_) {
            return;
        }
        stopping_ = true;
        std::error_code ignored;
        ignored = acceptor_.close(ignored);
    }

    void request_start(std::shared_ptr<const listener_activation_gate> activation_gate = nullptr) {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_) {
            throw std::runtime_error("edge HTTPS listener worker is stopping");
        }
        start_requested_ = true;
        const auto self = shared_from_this();
        try {
            if (!owner_.post([self, gate = std::move(activation_gate)] {
                           if (self->started_) {
                               return;
                           }
                           self->started_ = true;
                           self->activation_gate_ = gate;
                           self->start();
                       })
                    .accepted()) {
                throw std::runtime_error("edge HTTPS listener worker is stopping");
            }
        } catch (...) {
            start_requested_ = false;
            throw;
        }
    }

    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint() const {
        return acceptor_.local_endpoint();
    }

    void request_stop() noexcept {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_) {
            return;
        }
        if (!start_requested_) {
            stopping_ = true;
            std::error_code ignored;
            ignored = acceptor_.close(ignored);
            loop_stop_complete_ = true;
            return;
        }
        if (owner_.is_current()) {
            stop();
            return;
        }
        asio::post(owner_.executor(), [weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->stop();
            }
        });
    }

private:
    // Socket cancellation and accept initiation must stay on the owning loop.
    ruvia::task<void> stop_and_wait() {
        {
            std::lock_guard lock(lifecycle_mutex_);
            stop();
        }
        while (pending_operations_ != 0 || active_connections_ != 0) {
            co_await stop_signal_.wait();
        }
        std::lock_guard lock(lifecycle_mutex_);
        loop_stop_complete_ = true;
    }

    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        activation_timer_.cancel(ignored);
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
        for (const auto& weak : connections_) {
            if (const auto connection = weak.lock()) {
                connection->close();
            }
        }
    }

private:
    void start() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        if (activation_gate_ && !activation_gate_->active()) {
            const auto self = shared_from_this();
            bool operation_started = false;
            try {
                activation_timer_.expires_after(std::chrono::milliseconds(1));
                ++pending_operations_;
                operation_started = true;
                activation_timer_.async_wait([self](const std::error_code& error) {
                    self->operation_completed();
                    if (!error) {
                        self->start();
                    }
                });
            } catch (...) {
                if (operation_started) {
                    operation_completed();
                }
                stop();
                return;
            }
            return;
        }
        accept();
    }

    // Accept completions and connection retirement run on the owning loop.
    void operation_completed() noexcept {
        --pending_operations_;
        stop_signal_.notify();
    }

    void connection_retired() noexcept {
        --active_connections_;
        stop_signal_.notify();
    }

    void accept() {
        if (stopping_ || !acceptor_.is_open()) {
            return;
        }
        std::shared_ptr<asio::ip::tcp::socket> socket;
        std::shared_ptr<https_listener> self;
        try {
            socket = std::make_shared<asio::ip::tcp::socket>(owner_.io_context());
            self = shared_from_this();
        } catch (...) {
            stop();
            return;
        }
        ++pending_operations_;
        try {
            acceptor_.async_accept(
                *socket,
                asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                    self->operation_completed();
                    if (!error && !self->stopping_) {
                        const auto contexts = self->tls_contexts_.current();
                        if (contexts && !contexts->empty()) {
                            std::shared_ptr<https_connection> connection;
                            try {
                                connection = std::make_shared<https_connection>(
                                    [weak = self->weak_from_this()] {
                                        if (const auto listener = weak.lock()) {
                                            listener->connection_retired();
                                        }
                                    });
                                ++self->active_connections_;
                                std::erase_if(self->connections_, [](const auto& active) {
                                    return active.expired();
                                });
                                self->connections_.push_back(connection);
                                auto session = std::make_shared<tls_session>(
                                    std::move(*socket), self->runtime_, self->health_, contexts,
                                    self->metrics_, self->clients_, self->logs_,
                                    self->origin_connections_, self->request_buffers_,
                                    self->response_buffers_, ++self->sequence_, connection);
                                session->start();
                            } catch (...) {
                                if (connection) {
                                    connection->close();
                                }
                                connection.reset();
                            }
                        }
                    }
                    if (self->acceptor_.is_open()) {
                        self->accept();
                    }
                }));
        } catch (...) {
            operation_completed();
            stop();
        }
    }

    ruvia::event_loop owner_;
    // Borrowed by stop_signal_; declared first so it outlives every signal wait.
    ruvia::worker_handle owner_worker_;
    std::mutex lifecycle_mutex_;
    bool start_requested_{};
    bool loop_stop_complete_{};
    runtime_state& runtime_;
    origin_health_registry& health_;
    tls_context_registry& tls_contexts_;
    runtime_metrics& metrics_;
    connected_clients& clients_;
    node_log_buffer::producer& logs_;
    origin_connection_pool& origin_connections_;
    buffered_bytes_budget& request_buffers_;
    buffered_bytes_budget& response_buffers_;
    asio::ip::tcp::acceptor acceptor_;
    asio::steady_timer activation_timer_;
    ruvia::event_loop_stop_registration stop_registration_;
    ruvia::worker_signal stop_signal_;
    std::size_t pending_operations_{};
    std::size_t active_connections_{};
    std::vector<std::weak_ptr<https_connection>> connections_;
    bool stopping_{};
    std::shared_ptr<const listener_activation_gate> activation_gate_;
    std::uint64_t sequence_{};
    bool started_{};  // Accessed only by the owning event loop.
};

}  // namespace flexedge::node
