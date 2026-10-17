#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>
#include <ruvia/core/worker_signal.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/steady_timer.hpp>

#if defined(SO_REUSEPORT) && !defined(_WIN32)
#include <sys/socket.h>
#endif

#include "node/data/http_protocol.h"
#include "node/data/http_session.h"
#include "node/data/tls_context.h"

namespace flexedge::node {

// Completes the server TLS handshake of one accepted connection and hands the stream to an
// HTTP session.
class tls_handshake final : public std::enable_shared_from_this<tls_handshake> {
public:
    using stream = asio::ssl::stream<asio::ip::tcp::socket>;

    tls_handshake(asio::ip::tcp::socket socket, const http_session_context& context,
        std::shared_ptr<tls_context_set> contexts, std::uint64_t sequence,
        std::shared_ptr<edge_connection> connection)
        : stream_(std::move(socket), contexts->default_context()),
          context_(context),
          contexts_(std::move(contexts)),
          timer_(stream_.get_executor()),
          sequence_(sequence),
          connection_(std::move(connection)) {
        capture_tls_client_hello(stream_.native_handle(), client_hello_);
    }

    void start() {
        connection_->set_close([weak = weak_from_this()] {
            if (const auto self = weak.lock()) {
                self->close();
            }
        });
        const auto self = shared_from_this();
        try {
            timer_.expires_after(std::chrono::seconds(10));
            timer_.async_wait([self](const std::error_code& error) {
                if (!error) {
                    self->close();
                }
            });
            stream_.async_handshake(asio::ssl::stream_base::server,
                [self](const std::error_code& error) { self->handshake_completed(error); });
        } catch (...) {
            close();
            throw;
        }
    }

    void close() noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
        ignored = stream_.lowest_layer().close(ignored);
    }

private:
    void handshake_completed(const std::error_code& error) noexcept {
        std::error_code ignored;
        timer_.cancel(ignored);
        if (error) {
            close();
            return;
        }
        try {
            release_tls_client_hello(stream_.native_handle());
            connection_->retain_tls_context(contexts_, std::move(client_hello_.server_name_));
            auto session = std::make_shared<basic_http_session<stream>>(std::move(stream_), context_,
                sequence_, connection_, true, std::move(client_hello_.fingerprint_));
            try {
                session->start();
            } catch (...) {
                session->close();
            }
        } catch (...) {
            close();
        }
    }

    stream stream_;
    http_session_context context_;
    std::shared_ptr<tls_context_set> contexts_;
    asio::steady_timer timer_;
    std::uint64_t sequence_{};
    tls_client_hello client_hello_;
    std::shared_ptr<edge_connection> connection_;
};

// Accepts edge connections for one endpoint on one data worker. Plain HTTP connections go
// straight to a session; HTTPS connections complete their TLS handshake first. A listener
// binds when created, starts once the worker has accepted it (request_start) and the
// configuration that added it is active (activate), and stops with request_stop or its
// worker, waiting for every connection it admitted.
class edge_listener final : public std::enable_shared_from_this<edge_listener> {
public:
    // `tls_contexts` selects HTTPS; nullptr serves plain HTTP.
    [[nodiscard]] static std::shared_ptr<edge_listener> create(ruvia::event_loop owner,
        const http_session_context& context, const asio::ip::tcp::endpoint& endpoint,
        tls_context_registry* tls_contexts) {
        auto listener = std::shared_ptr<edge_listener>(
            new edge_listener(std::move(owner), context, endpoint, tls_contexts));
        listener->stop_registration_ = listener->owner_.on_stop(
            [weak = std::weak_ptr<edge_listener>(listener)]() -> ruvia::task<void> {
                if (const auto self = weak.lock()) {
                    co_await self->stop_and_wait();
                }
                co_return;
            });
        return listener;
    }

    edge_listener(const edge_listener&) = delete;
    edge_listener& operator=(const edge_listener&) = delete;

    ~edge_listener() {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_ || start_requested_) {
            return;
        }
        stopping_ = true;
        std::error_code ignored;
        ignored = acceptor_.close(ignored);
    }

    // Hands the listener to its worker; throws when the worker no longer accepts work.
    void request_start() {
        std::lock_guard lock(lifecycle_mutex_);
        if (loop_stop_complete_) {
            throw std::runtime_error(std::string(name()) + " listener worker is stopping");
        }
        start_requested_ = true;
        try {
            if (!owner_.post([self = shared_from_this()] {
                           self->started_ = true;
                           self->begin_accepting();
                       })
                    .accepted()) {
                throw std::runtime_error(std::string(name()) + " listener worker is stopping");
            }
        } catch (...) {
            start_requested_ = false;
            throw;
        }
    }

    // Tells the worker to begin accepting once the listener's configuration is active.
    void activate() noexcept {
        try {
            asio::post(owner_.executor(), [self = shared_from_this()] {
                self->activated_ = true;
                self->begin_accepting();
            });
        } catch (...) {
            request_stop();
        }
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

    [[nodiscard]] asio::ip::tcp::endpoint local_endpoint() const {
        return acceptor_.local_endpoint();
    }

private:
    edge_listener(ruvia::event_loop owner, const http_session_context& context,
        const asio::ip::tcp::endpoint& endpoint, tls_context_registry* tls_contexts)
        : owner_(std::move(owner)),
          owner_worker_(owner_.handle()),
          context_(context),
          tls_contexts_(tls_contexts),
          acceptor_(owner_.io_context()),
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
                    std::string("could not enable ") + name() + " SO_REUSEPORT");
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
            throw std::system_error(error, std::string("could not start ") + name() + " listener");
        }
    }

    [[nodiscard]] const char* name() const noexcept {
        return tls_contexts_ == nullptr ? "edge HTTP" : "edge HTTPS";
    }

public:
    // Owner loop. Retired listeners remain owned until their last callback has returned.
    [[nodiscard]] bool stopped() const noexcept {
        return stopping_ && pending_operations_ == 0 && active_connections_ == 0;
    }

    ruvia::task<void> stop_and_wait() {
        if (!owner_.is_current()) {
            throw std::logic_error("listener cleanup must run on its owner loop");
        }
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

private:
    void stop() noexcept {
        stopping_ = true;
        std::error_code ignored;
        ignored = acceptor_.cancel(ignored);
        ignored = acceptor_.close(ignored);
        for (const auto& weak : connections_) {
            if (const auto connection = weak.lock()) {
                connection->close();
            }
        }
    }

    // Owning loop. Starts the accept loop once both start and activation have arrived.
    void begin_accepting() {
        if (!started_ || !activated_ || accepting_) {
            return;
        }
        accepting_ = true;
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
        std::shared_ptr<edge_listener> self;
        try {
            socket = std::make_shared<asio::ip::tcp::socket>(owner_.io_context());
            self = shared_from_this();
        } catch (...) {
            stop();
            return;
        }
        ++pending_operations_;
        try {
            acceptor_.async_accept(*socket,
                asio::bind_executor(owner_.executor(), [self, socket](const std::error_code& error) {
                    self->operation_completed();
                    if (!error && !self->stopping_) {
                        self->admit(std::move(*socket));
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

    // A connection that cannot be admitted is closed with its socket.
    void admit(asio::ip::tcp::socket socket) noexcept {
        std::shared_ptr<tls_context_set> contexts;
        if (tls_contexts_ != nullptr) {
            contexts = tls_contexts_->current();
            if (!contexts || contexts->empty()) {
                return;
            }
        }
        std::shared_ptr<edge_connection> connection;
        try {
            connection = std::make_shared<edge_connection>([weak = weak_from_this()] {
                if (const auto listener = weak.lock()) {
                    listener->connection_retired();
                }
            });
            ++active_connections_;
            std::erase_if(connections_, [](const auto& active) { return active.expired(); });
            connections_.push_back(connection);
            if (contexts) {
                std::make_shared<tls_handshake>(std::move(socket), context_, std::move(contexts),
                    ++sequence_, connection)
                    ->start();
            } else {
                auto session = std::make_shared<basic_http_session<asio::ip::tcp::socket>>(
                    std::move(socket), context_, ++sequence_, connection, false);
                try {
                    session->start();
                } catch (...) {
                    session->close();
                }
            }
        } catch (...) {
            if (connection) {
                connection->close();
            }
        }
    }

    ruvia::event_loop owner_;
    // Borrowed by stop_signal_; declared first so it outlives every signal wait.
    ruvia::worker_handle owner_worker_;
    std::mutex lifecycle_mutex_;
    bool start_requested_{};
    bool loop_stop_complete_{};
    http_session_context context_;
    tls_context_registry* tls_contexts_{};
    asio::ip::tcp::acceptor acceptor_;
    ruvia::event_loop_stop_registration stop_registration_;
    ruvia::worker_signal stop_signal_;
    std::size_t pending_operations_{};
    std::size_t active_connections_{};
    std::vector<std::weak_ptr<edge_connection>> connections_;
    std::uint64_t sequence_{};
    bool stopping_{};
    // Accessed only by the owning event loop.
    bool started_{};
    bool activated_{};
    bool accepting_{};
};

}  // namespace flexedge::node
