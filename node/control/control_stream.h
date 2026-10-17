#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/websocket_client.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

class control_stream_ended final : public std::exception {
public:
    [[nodiscard]] const char* what() const noexcept override {
        return "control stream ended";
    }
};

inline ruvia::task<v2::ServerEnvelope>
read_server_envelope(const ruvia::websocket_client_handle& connection, bool reject_error = true) {
    const auto message = co_await connection.read();
    if (!message) {
        throw control_stream_ended{};
    }
    if (!message->binary()) {
        throw std::runtime_error("control plane must send protobuf binary messages");
    }
    if (message->payload().size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("control plane protobuf message is too large");
    }
    v2::ServerEnvelope envelope;
    if (!envelope.ParseFromArray(message->payload().data(),
            static_cast<int>(message->payload().size()))) {
        throw std::runtime_error("control plane sent invalid protobuf");
    }
    if (reject_error && envelope.has_error()) {
        throw std::runtime_error("control plane error: " + envelope.error().message());
    }
    co_return envelope;
}

class control_request_error final : public std::runtime_error {
public:
    explicit control_request_error(const v2::Error& error)
        : std::runtime_error("control plane error: " + error.message()),
          retryable_(error.retryable()) {}

    [[nodiscard]] bool retryable() const noexcept {
        return retryable_;
    }

private:
    bool retryable_;
};

enum class control_request_priority {
    control,
    logs,
};

// One authenticated WebSocket owns one read lane and one write lane. Only
// bounded in-flight requests live here; messages and callbacks never cross
// the control worker boundary. A log acknowledgement cannot hold the lane
// used by heartbeat, configuration, apply confirmation, or release requests.
class control_stream final {
public:
    static constexpr std::size_t maximum_requests{8};
    static constexpr std::size_t maximum_control_writes{8};
    static constexpr std::size_t maximum_log_writes{1};

    class request final {
    public:
        request(const request&) = delete;
        request& operator=(const request&) = delete;
        request(request&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              id_(std::move(other.id_)),
              replies_(std::move(other.replies_)) {}
        request& operator=(request&&) = delete;
        ~request() {
            if (owner_) {
                owner_->retire(id_);
            }
        }

        [[nodiscard]] std::string_view id() const noexcept {
            return id_;
        }

        ruvia::task<v2::ServerEnvelope> receive(std::chrono::seconds timeout = std::chrono::seconds(40)) {
            auto result = co_await replies_.receive_for(timeout, owner_->get_stop_token());
            if (!result.has_value()) {
                owner_->rethrow_failure();
                if (result.status() == ruvia::worker_wait_status::timed_out) {
                    throw ruvia::websocket_client_error(ruvia::websocket_client_error::code_type::timeout,
                        "control plane request acknowledgement timed out");
                }
                throw control_stream_ended{};
            }
            auto envelope = std::move(result).take_value();
            if (envelope.has_error()) {
                throw control_request_error(envelope.error());
            }
            co_return envelope;
        }

    private:
        friend class control_stream;
        request(control_stream& owner, std::string id, ruvia::channel_receiver<v2::ServerEnvelope> replies)
            : owner_(&owner),
              id_(std::move(id)),
              replies_(std::move(replies)) {}

        control_stream* owner_;
        std::string id_;
        ruvia::channel_receiver<v2::ServerEnvelope> replies_;
    };

    control_stream(const ruvia::worker_handle& worker, const ruvia::websocket_client_handle& connection,
        ruvia::stop_token stop)
        : worker_(worker),
          connection_(connection),
          operations_(worker_),
          stop_(ruvia::combine_stop_tokens(std::move(stop), operations_.get_stop_token())) {
        auto [sender, receiver] = ruvia::make_channel<bool>(worker_, {.capacity_ = 1});
        writes_ready_ = std::move(sender);
        writes_waiting_.emplace(std::move(receiver));
    }

    control_stream(const control_stream&) = delete;
    control_stream& operator=(const control_stream&) = delete;

    void start() {
        if (started_) {
            throw std::logic_error("control stream already started");
        }
        started_ = true;
        operations_.spawn(read_replies());
        operations_.spawn(write_requests());
    }

    [[nodiscard]] ruvia::stop_token get_stop_token() const {
        return stop_;
    }
    [[nodiscard]] const ruvia::worker_handle& worker() const noexcept {
        return worker_;
    }

    request expect(std::string id) {
        rethrow_failure();
        if (stop_.stop_requested()) {
            throw control_stream_ended{};
        }
        if (id.empty() || replies_.size() >= maximum_requests || replies_.contains(id)) {
            throw std::runtime_error("control stream request capacity or identifier conflict");
        }
        auto [sender, receiver] = ruvia::make_channel<v2::ServerEnvelope>(worker_, {.capacity_ = 2});
        replies_.emplace(id, std::move(sender));
        return request(*this, std::move(id), std::move(receiver));
    }

    request send(v2::ClientEnvelope& envelope, control_request_priority priority = control_request_priority::control) {
        envelope.set_request_id("request-" + std::to_string(++sequence_));
        auto pending = expect(envelope.request_id());
        auto& queue = priority == control_request_priority::control ? control_writes_ : log_writes_;
        const auto limit = priority == control_request_priority::control ? maximum_control_writes : maximum_log_writes;
        if (queue.size() >= limit) {
            throw std::runtime_error("control stream outbound capacity exceeded");
        }
        queue.push_back({.request_id_ = envelope.request_id(), .bytes_ = serialize_artifact(envelope)});
        (void)writes_ready_.send(true);
        return pending;
    }

    void spawn_owned(ruvia::task<void> operation) {
        operations_.spawn(run_owned(std::move(operation)));
    }

    ruvia::task<void> shutdown() {
        operations_.request_stop();
        connection_.abort();
        writes_ready_.close();
        for (const auto& [_, sender] : replies_) {
            sender.close();
        }
        co_await operations_.join();
        control_writes_.clear();
        log_writes_.clear();
    }

private:
    struct queued_request final {
        std::string request_id_;
        std::string bytes_;
    };

    void retire(const std::string& id) {
        replies_.erase(id);
    }

    void rethrow_failure() const {
        if (failure_) {
            std::rethrow_exception(failure_);
        }
    }

    void fail(std::exception_ptr failure) {
        if (!stop_.stop_requested() && !failure_) {
            failure_ = std::move(failure);
        }
        operations_.request_stop();
        connection_.abort();
        writes_ready_.close();
        for (const auto& [_, sender] : replies_) {
            sender.close();
        }
    }

    ruvia::task<void> run_owned(ruvia::task<void> operation) {
        try {
            co_await std::move(operation);
        } catch (...) {
            // Keep the original background error; cancellation of the reader
            // must not replace a failed release/cache operation with EOF.
            fail(std::current_exception());
        }
    }

    ruvia::task<void> read_replies() {
        try {
            const auto connection = connection_.with_options({.stop_token_ = stop_});
            while (!stop_.stop_requested()) {
                auto envelope = co_await read_server_envelope(connection, false);
                const auto found = replies_.find(envelope.request_id());
                // A timed-out request may receive its final reply later. Its
                // identifier is never reused in this session, so it is safe
                // to discard that reply instead of feeding the next request.
                if (found == replies_.end()) {
                    continue;
                }
                const auto result = found->second.send(std::move(envelope));
                if (!result.accepted()) {
                    throw std::runtime_error("control plane exceeded a request reply budget");
                }
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    ruvia::task<void> write_requests() {
        try {
            const auto connection = connection_.with_options({.stop_token_ = stop_});
            while (!stop_.stop_requested()) {
                if (control_writes_.empty() && log_writes_.empty()) {
                    if (!(co_await writes_waiting_->receive(stop_)).has_value()) {
                        co_return;
                    }
                    continue;
                }
                auto& queue = control_writes_.empty() ? log_writes_ : control_writes_;
                auto next = std::move(queue.front());
                queue.pop_front();
                if (!replies_.contains(next.request_id_)) {
                    continue;
                }
                co_await connection.binary(next.bytes_);
            }
        } catch (...) {
            fail(std::current_exception());
        }
    }

    ruvia::worker_handle worker_;
    ruvia::websocket_client_handle connection_;
    ruvia::task_scope operations_;
    ruvia::stop_token stop_;
    ruvia::channel_sender<bool> writes_ready_;
    std::optional<ruvia::channel_receiver<bool>> writes_waiting_;
    std::unordered_map<std::string, ruvia::channel_sender<v2::ServerEnvelope>> replies_;
    std::deque<queued_request> control_writes_;
    std::deque<queued_request> log_writes_;
    std::exception_ptr failure_;
    std::uint64_t sequence_{};
    bool started_{};
};

}  // namespace flexedge::node
