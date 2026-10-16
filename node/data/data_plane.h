#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "node/data/edge_listener.h"
#include "node/data/health_supervisor.h"
#include "node/data/worker_metrics.h"
#include "node/runtime/compiled_config.h"
#include "node/runtime/connected_clients.h"
#include "node/runtime/log_producer.h"

namespace flexedge::node {

// What the data plane reports to the control loop: immutable copies only.
struct data_plane_report final {
    // Empty when a worker could not answer in time.
    std::optional<data_plane_load> load_;
    std::shared_ptr<const origin_health_snapshot> origin_health_;
};

class prepared_data_plane_reload final {
public:
    prepared_data_plane_reload(prepared_data_plane_reload&&) noexcept = default;
    prepared_data_plane_reload& operator=(prepared_data_plane_reload&&) noexcept = default;
    prepared_data_plane_reload(const prepared_data_plane_reload&) = delete;
    prepared_data_plane_reload& operator=(const prepared_data_plane_reload&) = delete;

    [[nodiscard]] std::size_t added_listeners() const noexcept {
        return additions_.size() + https_additions_.size();
    }
    [[nodiscard]] std::size_t retired_listeners() const noexcept {
        return retired_.size() + https_retired_.size();
    }

private:
    friend class data_plane;

    prepared_data_plane_reload() = default;

    std::unordered_map<std::string, std::shared_ptr<edge_listener>> listeners_;
    std::vector<std::pair<std::string, std::shared_ptr<edge_listener>>> additions_;
    std::unordered_map<std::string, std::shared_ptr<edge_listener>> https_listeners_;
    std::vector<std::pair<std::string, std::shared_ptr<edge_listener>>> https_additions_;
    std::vector<std::shared_ptr<edge_listener>> retired_;
    std::vector<std::shared_ptr<edge_listener>> https_retired_;
    std::optional<origin_health_state::prepared> health_;
    std::shared_ptr<const tls_context_set> tls_contexts_;
    std::shared_ptr<const compiled_config> config_;
    bool primed_{};
};

class data_plane final {
public:
    data_plane(ruvia::event_loop_pool& loops, ruvia::event_loop owner, runtime_state& runtime,
        ruvia::channel_sender<owned_log_batch> logs)
        : loops_(loops),
          owner_(std::move(owner)),
          runtime_(runtime),
          health_supervisor_(origin_health_supervisor::create(owner_, runtime_, health_probe_tls_)),
          stop_registration_(owner_.on_stop([this]() -> ruvia::task<void> {
              {
                  std::lock_guard lock(stop_mutex_);
                  owner_stopping_ = true;
                  stop_queued_ = true;
              }
              stop_on_owner();
              co_await health_supervisor_->stop_and_wait();
          })) {
        workers_.reserve(loops_.loop_count());
        counters_.reserve(loops_.loop_count());
        for (std::size_t index = 0; index < loops_.loop_count(); ++index) {
            workers_.push_back(std::make_unique<worker_state>(loops_.loop(index), logs));
            counters_.push_back({.loop_ = loops_.loop(index),
                .metrics_ = &workers_.back()->metrics_,
                .clients_ = &workers_.back()->clients_,
                .logs_ = &workers_.back()->logs_});
        }
    }

    ~data_plane() {
        stop_on_owner();
    }

    void start() {
        health_supervisor_->request_start();
    }

    // Runs on the owner loop. Each worker copies its own counters on its loop; origin health
    // is the snapshot the supervisor last published.
    [[nodiscard]] ruvia::task<data_plane_report> report(ruvia::stop_token stop) {
        if (!owner_.is_current()) {
            throw std::logic_error("data plane report must run on the data plane owner");
        }
        data_plane_report result;
        result.load_ = co_await collect_worker_load(owner_.handle(), counters_, std::move(stop));
        result.origin_health_ = health_supervisor_->snapshot();
        co_return result;
    }

    // Runs on the owner loop. Completes with true once every worker has closed its last
    // connection, with false when `timeout` elapses first.
    [[nodiscard]] ruvia::task<bool> wait_drained(std::chrono::steady_clock::duration timeout) {
        if (!owner_.is_current()) {
            throw std::logic_error("data plane drain must be awaited on the data plane owner");
        }
        co_return co_await wait_workers_drained(owner_.handle(), counters_, timeout);
    }

    void request_stop_accepting() noexcept {
        std::lock_guard lock(stop_mutex_);
        if (owner_stopping_ || stop_queued_) {
            return;
        }
        stop_queued_ = true;
        if (owner_.is_current()) {
            stop_on_owner();
            return;
        }
        asio::post(owner_.executor(), [this] { stop_on_owner(); });
    }

    [[nodiscard]] prepared_data_plane_reload prepare(std::shared_ptr<const compiled_config> config) {
        if (!owner_.is_current()) {
            throw std::logic_error("data plane reload must run on its owner worker");
        }
        if (!config) {
            throw std::invalid_argument("data plane config cannot be empty");
        }
        runtime_.validate_next(*config);
        prepared_data_plane_reload reload;
        reload.config_ = std::move(config);
        reload.tls_contexts_ = std::make_shared<const tls_context_set>(*reload.config_);
        if (!reload.config_->enabled()) {
            return finish_prepare(std::move(reload));
        }
        for (const auto& endpoint : reload.config_->endpoints()) {
            for (std::size_t worker_index = 0; worker_index < loops_.loop_count(); ++worker_index) {
                auto key = listener_key(endpoint, worker_index);
                const auto prepared = reload.listeners_.find(key);
                const auto existing = listeners_.find(key);
                if (prepared != reload.listeners_.end()) {
                    // Multiple advertised addresses of the same family share the wildcard
                    // listener for this port.
                } else if (existing != listeners_.end()) {
                    reload.listeners_.emplace(std::move(key), existing->second);
                } else {
                    reload.additions_.emplace_back(
                        key, edge_listener::create(loops_.loop(worker_index),
                                 session_context(worker_index),
                                 asio::ip::tcp::endpoint(bind_address(endpoint.ip_address()),
                                     static_cast<std::uint16_t>(endpoint.http_port())),
                                 nullptr));
                    reload.listeners_.emplace(key, reload.additions_.back().second);
                }
                if (reload.tls_contexts_->empty()) {
                    continue;
                }
                auto https_key =
                    listener_key(endpoint.ip_address(), endpoint.https_port(), worker_index);
                const auto prepared_https = reload.https_listeners_.find(https_key);
                const auto existing_https = https_listeners_.find(https_key);
                if (prepared_https != reload.https_listeners_.end()) {
                    // See the HTTP listener case above.
                } else if (existing_https != https_listeners_.end()) {
                    reload.https_listeners_.emplace(std::move(https_key), existing_https->second);
                } else {
                    reload.https_additions_.emplace_back(
                        https_key, edge_listener::create(loops_.loop(worker_index),
                                       session_context(worker_index),
                                       asio::ip::tcp::endpoint(bind_address(endpoint.ip_address()),
                                           static_cast<std::uint16_t>(endpoint.https_port())),
                                       &tls_contexts_));
                    reload.https_listeners_.emplace(https_key, reload.https_additions_.back().second);
                }
            }
        }
        return finish_prepare(std::move(reload));
    }

    void prime(prepared_data_plane_reload& reload) {
        if (!owner_.is_current()) {
            throw std::logic_error("data plane reload must run on its owner worker");
        }
        if (reload.primed_) {
            throw std::logic_error("data plane reload is already primed");
        }
        try {
            for (const auto& [_, listener] : reload.additions_) {
                listener->request_start();
            }
            for (const auto& [_, listener] : reload.https_additions_) {
                listener->request_start();
            }
            reload.primed_ = true;
        } catch (...) {
            abort(reload);
            throw;
        }
    }

    void activate(prepared_data_plane_reload reload) noexcept {
        health_supervisor_->activate(std::move(*reload.health_));
        tls_contexts_.publish(std::move(reload.tls_contexts_));
        runtime_.publish(std::move(reload.config_));
        listeners_.swap(reload.listeners_);
        https_listeners_.swap(reload.https_listeners_);
        for (const auto& [_, listener] : reload.additions_) {
            listener->activate();
        }
        for (const auto& [_, listener] : reload.https_additions_) {
            listener->activate();
        }
        for (const auto& listener : reload.retired_) {
            listener->request_stop();
        }
        for (const auto& listener : reload.https_retired_) {
            listener->request_stop();
        }
    }

    void abort(prepared_data_plane_reload& reload) noexcept {
        for (const auto& [_, listener] : reload.additions_) {
            listener->request_stop();
        }
        for (const auto& [_, listener] : reload.https_additions_) {
            listener->request_stop();
        }
        reload.primed_ = false;
    }

private:
    struct worker_state final {
        worker_state(ruvia::event_loop loop, ruvia::channel_sender<owned_log_batch> logs)
            : origin_connections_(loop.executor(), origin_tls_),
              logs_(std::move(loop), std::move(logs)) {}

        worker_metrics metrics_;
        connected_clients clients_;
        origin_tls_context origin_tls_;
        origin_connection_pool origin_connections_;
        log_producer logs_;
    };

    [[nodiscard]] http_session_context session_context(std::size_t worker_index) noexcept {
        auto& worker = *workers_[worker_index];
        return {.runtime_ = runtime_,
            .health_ = *health_supervisor_,
            .metrics_ = worker.metrics_,
            .clients_ = worker.clients_,
            .logs_ = worker.logs_,
            .origin_connections_ = worker.origin_connections_,
            .request_buffers_ = request_buffers_,
            .response_buffers_ = response_buffers_};
    }

    [[nodiscard]] prepared_data_plane_reload finish_prepare(prepared_data_plane_reload reload) {
        for (const auto& [key, listener] : listeners_) {
            if (!reload.listeners_.contains(key)) {
                reload.retired_.push_back(listener);
            }
        }
        for (const auto& [key, listener] : https_listeners_) {
            if (!reload.https_listeners_.contains(key)) {
                reload.https_retired_.push_back(listener);
            }
        }
        origin_health_key_set origin_keys;
        for (const auto* website : reload.config_->websites()) {
            for (const auto& origin : website->origins()) {
                origin_keys.emplace(origin_health_key{
                    .website_id_ = std::string(website->id()), .origin_id_ = std::string(origin.id())});
            }
        }
        reload.health_ = health_supervisor_->prepare(origin_keys);
        return reload;
    }

    static std::string listener_key(const v2::Endpoint& endpoint, std::size_t worker_index) {
        return listener_key(endpoint.ip_address(), endpoint.http_port(), worker_index);
    }

    static std::string listener_key(std::string_view address, std::uint32_t port,
        std::size_t worker_index) {
        const auto parsed = asio::ip::make_address(address);
        return std::string(parsed.is_v6() ? "v6:" : "v4:") + std::to_string(port) + '#' +
               std::to_string(worker_index);
    }

    static asio::ip::address bind_address(std::string_view advertised_address) {
        return asio::ip::make_address(advertised_address).is_v6()
                   ? asio::ip::address{asio::ip::address_v6::any()}
                   : asio::ip::address{asio::ip::address_v4::any()};
    }

    void stop_on_owner() noexcept {
        for (const auto& [_, listener] : listeners_) {
            listener->request_stop();
        }
        listeners_.clear();
        for (const auto& [_, listener] : https_listeners_) {
            listener->request_stop();
        }
        https_listeners_.clear();
    }

    ruvia::event_loop_pool& loops_;
    ruvia::event_loop owner_;
    std::mutex stop_mutex_;
    bool stop_queued_{};
    bool owner_stopping_{};
    runtime_state& runtime_;
    origin_tls_context health_probe_tls_;
    tls_context_registry tls_contexts_;
    std::shared_ptr<origin_health_supervisor> health_supervisor_;
    buffered_bytes_budget request_buffers_{maximum_request_buffer_bytes};
    buffered_bytes_budget response_buffers_{maximum_response_buffer_bytes};
    ruvia::event_loop_stop_registration stop_registration_;
    std::vector<std::unique_ptr<worker_state>> workers_;
    std::vector<worker_counters> counters_;
    std::unordered_map<std::string, std::shared_ptr<edge_listener>> listeners_;
    std::unordered_map<std::string, std::shared_ptr<edge_listener>> https_listeners_;
};

}  // namespace flexedge::node
