#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/post.hpp>

#include "node/data/edge_listener.h"
#include "node/data/health_supervisor.h"
#include "node/data/worker_metrics.h"

namespace flexedge::node {

enum class data_worker_operation { prepare,
    activate,
    finalize,
    rollback,
    discard,
    report,
    drain,
    stop_accepting,
    retired };

// Cross-loop messages contain owned immutable business values, never runtime objects,
// listener handles, completion callbacks or channels back into another worker.
struct data_worker_request final {
    data_worker_operation operation_{};
    std::uint64_t sequence_{};
    std::uint64_t committed_sequence_{};
    std::shared_ptr<const compiled_config> config_{};
    std::chrono::steady_clock::duration timeout_{};
    std::chrono::steady_clock::time_point deadline_{std::chrono::steady_clock::time_point::max()};
};
using owned_data_worker_request = std::unique_ptr<const data_worker_request>;

struct data_worker_snapshot final {
    std::shared_ptr<const compiled_config> config_{};
    std::int64_t connection_count_{};
    std::uint64_t traffic_out_bytes_{};
    std::vector<connected_clients::address_key> addresses_{};
    log_production_snapshot logs_{};
    std::shared_ptr<const origin_health_snapshot> health_{};
    std::size_t request_buffer_limit_{};
    std::size_t request_buffer_bytes_{};
    std::size_t response_buffer_limit_{};
    std::size_t response_buffer_bytes_{};
};

struct data_worker_result final {
    data_worker_operation operation_{};
    std::uint64_t sequence_{};
    std::size_t worker_id_{};
    bool ok_{true};
    std::string error_{};
    std::size_t added_listeners_{};
    std::size_t retired_listeners_{};
    std::optional<data_worker_snapshot> snapshot_{};
};
using owned_data_worker_result = std::unique_ptr<const data_worker_result>;

// Identical on every data loop. Assembly retains its lifetime until the pool has joined;
// after start(), only this loop accesses the runtime below. The control loop holds channel
// endpoints, and sends/receives the immutable messages above.
class data_worker final {
public:
    data_worker(ruvia::event_loop loop, std::size_t identity,
        ruvia::channel_receiver<owned_data_worker_request> requests,
        ruvia::channel_sender<owned_data_worker_result> results,
        ruvia::channel_sender<owned_log_batch> logs, std::size_t worker_count)
        : loop_(std::move(loop)),
          worker_(loop_.handle()),
          identity_(identity),
          requests_(std::move(requests)),
          results_(std::move(results)),
          operations_(worker_),
          origin_connections_(loop_.executor(), origin_tls_),
          logs_(loop_, std::move(logs)),
          health_(origin_health_supervisor::create(loop_, runtime_, origin_tls_)),
          request_buffers_(maximum_request_buffer_bytes / worker_count),
          response_buffers_(maximum_response_buffer_bytes / worker_count),
          stop_registration_(loop_.on_stop([this]() -> ruvia::task<void> {
              stop_accepting();
              health_->request_stop();
              requests_.close();
              operations_.request_stop();
              std::exception_ptr failure;
              try {
                  co_await operations_.join();
              } catch (...) {
                  failure = std::current_exception();
              }
              for (const auto& listener : owned_listeners_) {
                  co_await listener->stop_and_wait();
              }
              owned_listeners_.clear();
              co_await health_->stop_and_wait();
              origin_connections_.close();
              committed_.reset();
              tls_contexts_.publish({});
              runtime_.publish({}, {});
              // Retirement acknowledges that this worker can no longer serve a tentative
              // configuration. Survivors still have to acknowledge rollback themselves.
              try {
                  reply({.operation_ = data_worker_operation::retired});
              } catch (...) {
                  // The control loop may already be retiring its receiver.
              }
              if (failure) {
                  std::rethrow_exception(failure);
              }
          })) {}

    data_worker(const data_worker&) = delete;
    data_worker& operator=(const data_worker&) = delete;

    // Process assembly only, after both pools and their shutdown guard are ready.
    void start() {
        if (!loop_.post([this] {
                      if (!loop_.accepting()) {
                          return;
                      }
                      operations_.spawn(run());
                      operations_.spawn(origin_connections_.watch_dns(loop_, operations_.get_stop_token()));
                      health_->request_start();
                  })
                .accepted()) {
            throw std::runtime_error("data worker is stopping");
        }
    }

    // Administrative stop only. Runtime mutation and cancellation stay on the owner loop.
    void request_stop_accepting() noexcept {
        try {
            (void)loop_.defer_cleanup([this] { stop_accepting(); });
        } catch (...) {
            // The loop's stop hook repeats cleanup when post admission has closed.
        }
    }

private:
    using listener_map = std::unordered_map<std::string, std::shared_ptr<edge_listener>>;

    struct prepared_reload final {
        ~prepared_reload() {
            if (!committed_) {
                for (const auto& listener : additions_) {
                    listener->request_stop();
                }
            }
        }

        std::uint64_t sequence_{};
        std::shared_ptr<const compiled_config> config_{};
        std::shared_ptr<const route_patterns> patterns_{};
        std::shared_ptr<tls_context_set> tls_contexts_{};
        std::optional<origin_health_state::prepared> health_{};
        listener_map listeners_{};
        std::vector<std::shared_ptr<edge_listener>> additions_{};
        std::vector<std::shared_ptr<edge_listener>> retired_{};
        bool committed_{};
        bool unchanged_{};
    };

    void reply(data_worker_result result) {
        result.worker_id_ = identity_;
        const auto sent = results_.send(std::make_unique<const data_worker_result>(std::move(result)));
        if (sent.status() == ruvia::channel_send_status::full) {
            throw std::runtime_error("data worker result channel is full");
        }
    }

    ruvia::task<void> run() {
        try {
            for (;;) {
                auto next = co_await requests_.receive(operations_.get_stop_token());
                if (!next.has_value()) {
                    if (!operations_.stop_requested() && loop_.accepting()) {
                        loop_.report_failure(std::make_exception_ptr(std::runtime_error("data worker command stream closed")));
                    }
                    co_return;
                }
                const auto request = std::move(next).take_value();
                std::erase_if(owned_listeners_, [](const auto& listener) { return listener->stopped(); });
                data_worker_result result{.operation_ = request->operation_, .sequence_ = request->sequence_};
                try {
                    if (committed_ && committed_->sequence_ <= request->committed_sequence_) {
                        finalize(committed_->sequence_);
                    }
                    switch (request->operation_) {
                        case data_worker_operation::prepare:
                            prepare(*request);
                            result.added_listeners_ = pending_->additions_.size();
                            result.retired_listeners_ = pending_->retired_.size();
                            break;
                        case data_worker_operation::activate:
                            require_accepting();
                            require_current(*request);
                            activate(request->sequence_);
                            break;
                        case data_worker_operation::finalize:
                            finalize(request->sequence_);
                            continue;
                        case data_worker_operation::rollback:
                            rollback(request->sequence_);
                            break;
                        case data_worker_operation::discard:
                            rollback(request->sequence_);
                            if (pending_ && pending_->sequence_ == request->sequence_) {
                                pending_.reset();
                            }
                            continue;
                        case data_worker_operation::report:
                            result.snapshot_ = snapshot();
                            break;
                        case data_worker_operation::drain:
                            operations_.spawn(report_drained(request->sequence_, request->timeout_));
                            continue;
                        case data_worker_operation::stop_accepting:
                            stop_accepting();
                            continue;
                        case data_worker_operation::retired:
                            throw std::logic_error("retirement is a worker result, not a request");
                    }
                } catch (const std::exception& error) {
                    result.ok_ = false;
                    result.error_ = error.what();
                }
                reply(std::move(result));
            }
        } catch (...) {
            loop_.report_failure(std::current_exception());
            throw;
        }
    }

    [[nodiscard]] http_session_context session_context() noexcept {
        return {.runtime_ = runtime_, .health_ = *health_, .metrics_ = metrics_, .clients_ = clients_, .logs_ = logs_, .origin_connections_ = origin_connections_, .request_buffers_ = request_buffers_, .response_buffers_ = response_buffers_};
    }

    void prepare(const data_worker_request& request) {
        require_accepting();
        require_current(request);
        if (committed_) {
            throw std::logic_error("data worker reload decision is missing");
        }
        if (!request.config_) {
            throw std::invalid_argument("data worker config cannot be empty");
        }
        if (pending_) {
            if (request.sequence_ <= pending_->sequence_) {
                throw std::logic_error("stale data worker reload");
            }
            // A newer attempt also recovers an abandoned prepare whose discard could not
            // enter a full mailbox. No prepared resource survives into the next attempt.
            pending_.reset();
        }
        runtime_.validate_next(*request.config_);
        auto reload = std::make_unique<prepared_reload>();
        reload->sequence_ = request.sequence_;
        reload->config_ = request.config_;
        reload->patterns_ = compile_route_patterns(*reload->config_);
        const auto active = runtime_.config();
        reload->unchanged_ = active && active->node_spec_digest() == reload->config_->node_spec_digest() &&
                             active->release_digest() == reload->config_->release_digest();
        reload->tls_contexts_ = reload->unchanged_ ? tls_contexts_.current() : std::make_shared<tls_context_set>(*reload->config_);
        if (reload->config_->enabled()) {
            for (const auto& endpoint : reload->config_->endpoints()) {
                prepare_listener(*reload, endpoint.ip_address(), endpoint.http_port(), false);
                if (!reload->tls_contexts_->empty()) {
                    prepare_listener(*reload, endpoint.ip_address(), endpoint.https_port(), true);
                }
            }
        }
        for (const auto& [key, listener] : listeners_) {
            if (!reload->listeners_.contains(key)) {
                reload->retired_.push_back(listener);
            }
        }
        if (!reload->unchanged_) {
            std::unordered_map<origin_health_key, std::pair<const v2::Website*, const v2::Origin*>, origin_health_key_hash, origin_health_key_equal> previous;
            if (active) {
                for (const auto* website : active->websites()) {
                    for (const auto& origin : website->origins()) {
                        previous.emplace(origin_health_key{.website_id_ = website->id(), .origin_id_ = origin.id()}, std::pair{website, &origin});
                    }
                }
            }
            origin_health_key_set keys;
            origin_health_key_set replaced;
            for (const auto* website : reload->config_->websites()) {
                for (const auto& origin : website->origins()) {
                    origin_health_key key{.website_id_ = website->id(), .origin_id_ = origin.id()};
                    if (const auto old = previous.find(key); old != previous.end()) {
                        const auto& [old_website, old_origin] = old->second;
                        if (website->enabled() != old_website->enabled() || origin.enabled() != old_origin->enabled() ||
                            website->health_check_enabled() != old_website->health_check_enabled() ||
                            website->health_check_interval_seconds() != old_website->health_check_interval_seconds() ||
                            make_origin_probe_config(*website, origin) != make_origin_probe_config(*old_website, *old_origin)) {
                            replaced.emplace(key);
                        }
                    }
                    keys.emplace(std::move(key));
                }
            }
            reload->health_ = health_->prepare(keys, replaced);
        }
        for (const auto& listener : reload->additions_) {
            listener->request_start();
        }
        require_current(request);
        pending_ = std::move(reload);
    }

    void prepare_listener(prepared_reload& reload, std::string_view address, std::uint32_t port, bool secure) {
        const auto parsed = asio::ip::make_address(address);
        auto key = std::string(secure ? "https:" : "http:") + (parsed.is_v6() ? "v6:" : "v4:") + std::to_string(port);
        if (reload.listeners_.contains(key)) {
            return;
        }
        if (const auto existing = listeners_.find(key); existing != listeners_.end()) {
            reload.listeners_.emplace(std::move(key), existing->second);
            return;
        }
        const auto bind = parsed.is_v6() ? asio::ip::address(asio::ip::address_v6::any())
                                         : asio::ip::address(asio::ip::address_v4::any());
        auto listener = edge_listener::create(loop_, session_context(),
            {bind, static_cast<std::uint16_t>(port)}, secure ? &tls_contexts_ : nullptr);
        owned_listeners_.push_back(listener);
        reload.additions_.push_back(listener);
        reload.listeners_.emplace(std::move(key), std::move(listener));
    }

    void activate(std::uint64_t sequence) {
        if (!pending_ || pending_->sequence_ != sequence) {
            throw std::logic_error("data worker reload is not prepared");
        }
        if (pending_->unchanged_) {
            // Reconnecting the control channel restores the same persisted configuration.
            // Keep in-flight probes, health streaks and TLS runtime for that exact state.
            auto previous = runtime_.config();
            auto previous_patterns = runtime_.patterns();
            runtime_.publish(std::move(pending_->config_), std::move(pending_->patterns_));
            pending_->config_ = std::move(previous);
            pending_->patterns_ = std::move(previous_patterns);
            pending_->committed_ = true;
            committed_ = std::move(pending_);
            return;
        }
        health_->activate(std::move(*pending_->health_));
        auto previous_tls = tls_contexts_.current();
        tls_contexts_.publish(std::move(pending_->tls_contexts_));
        pending_->tls_contexts_ = std::move(previous_tls);
        auto previous = runtime_.config();
        auto previous_patterns = runtime_.patterns();
        runtime_.publish(std::move(pending_->config_), std::move(pending_->patterns_));
        pending_->config_ = std::move(previous);
        pending_->patterns_ = std::move(previous_patterns);
        listeners_.swap(pending_->listeners_);
        for (const auto& listener : pending_->additions_) {
            listener->activate();
        }
        pending_->committed_ = true;
        committed_ = std::move(pending_);
    }

    void finalize(std::uint64_t sequence) noexcept {
        if (committed_ && committed_->sequence_ == sequence) {
            for (const auto& listener : committed_->retired_) {
                listener->request_stop();
            }
            committed_.reset();
            settled_sequence_ = (std::max)(settled_sequence_, sequence);
        }
    }

    void rollback(std::uint64_t sequence) noexcept {
        if (sequence <= settled_sequence_) {
            return;
        }
        settled_sequence_ = sequence;
        if (pending_ && pending_->sequence_ == sequence) {
            pending_.reset();
        }
        if (!committed_ || committed_->sequence_ != sequence) {
            return;
        }
        if (!committed_->unchanged_) {
            health_->activate(std::move(*committed_->health_));
            tls_contexts_.publish(std::move(committed_->tls_contexts_));
            listeners_.swap(committed_->listeners_);
        }
        runtime_.publish(std::move(committed_->config_), std::move(committed_->patterns_));
        committed_->committed_ = false;
        committed_.reset();
    }

    void require_accepting() const {
        if (stopping_ || !loop_.accepting()) {
            throw std::runtime_error("data worker is stopping");
        }
    }

    void require_current(const data_worker_request& request) const {
        if (request.sequence_ <= settled_sequence_) {
            throw std::logic_error("data worker reload is already settled");
        }
        if (std::chrono::steady_clock::now() >= request.deadline_) {
            throw std::runtime_error("data worker reload deadline expired");
        }
    }

    void stop_accepting() noexcept {
        stopping_ = true;
        pending_.reset();
        // Admission stop is not a transaction decision. The control loop may have just
        // committed globally; only its immutable decision can roll back or finalize.
        for (const auto& listener : owned_listeners_) {
            listener->request_stop();
        }
        listeners_.clear();
    }

    [[nodiscard]] data_worker_snapshot snapshot() {
        return {.config_ = runtime_.config(), .connection_count_ = metrics_.connection_count(), .traffic_out_bytes_ = metrics_.traffic_out_bytes(), .addresses_ = clients_.snapshot(), .logs_ = logs_.snapshot(), .health_ = health_->snapshot(), .request_buffer_limit_ = request_buffers_.maximum_bytes(), .request_buffer_bytes_ = request_buffers_.used_bytes(), .response_buffer_limit_ = response_buffers_.maximum_bytes(), .response_buffer_bytes_ = response_buffers_.used_bytes()};
    }

    ruvia::task<void> report_drained(std::uint64_t sequence, std::chrono::steady_clock::duration timeout) {
        auto [sender, receiver] = ruvia::make_channel<bool>(worker_, {.capacity_ = 1});
        metrics_.notify_when_drained(std::move(sender));
        const auto drained = co_await receiver.receive_for(timeout, operations_.get_stop_token());
        reply({.operation_ = data_worker_operation::drain, .sequence_ = sequence, .ok_ = drained.has_value()});
    }

    ruvia::event_loop loop_;
    ruvia::worker_handle worker_;
    std::size_t identity_;
    ruvia::channel_receiver<owned_data_worker_request> requests_;
    ruvia::channel_sender<owned_data_worker_result> results_;
    ruvia::task_scope operations_;
    runtime_state runtime_;
    worker_metrics metrics_;
    connected_clients clients_;
    origin_tls_context origin_tls_;
    origin_connection_pool origin_connections_;
    log_producer logs_;
    std::shared_ptr<origin_health_supervisor> health_;
    tls_context_registry tls_contexts_;
    buffered_bytes_budget request_buffers_;
    buffered_bytes_budget response_buffers_;
    listener_map listeners_;
    std::vector<std::shared_ptr<edge_listener>> owned_listeners_;
    std::unique_ptr<prepared_reload> pending_;
    std::unique_ptr<prepared_reload> committed_;
    std::uint64_t settled_sequence_{};
    bool stopping_{};
    ruvia::event_loop_stop_registration stop_registration_;
};

}  // namespace flexedge::node
