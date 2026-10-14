#pragma once

#include <ruvia/core/event_loop.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "node/data/health_supervisor.h"
#include "node/data/http_listener.h"
#include "node/data/https_listener.h"
#include "node/runtime/compiled_config.h"
#include "node/runtime/connected_clients.h"
#include "node/runtime/log_buffer.h"

namespace flexedge::node {

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

    std::unordered_map<std::string, std::shared_ptr<http_listener>> listeners_;
    std::vector<std::pair<std::string, std::shared_ptr<http_listener>>> additions_;
    std::unordered_map<std::string, std::shared_ptr<https_listener>> https_listeners_;
    std::vector<std::pair<std::string, std::shared_ptr<https_listener>>> https_additions_;
    std::vector<std::shared_ptr<http_listener>> retired_;
    std::vector<std::shared_ptr<https_listener>> https_retired_;
    std::optional<origin_health_registry::prepared_states> health_states_;
    std::shared_ptr<const tls_context_set> tls_contexts_;
    std::shared_ptr<listener_activation_gate> activation_gate_;
    std::shared_ptr<const compiled_config> config_;
    bool primed_{};
};

class data_plane final {
public:
    data_plane(ruvia::event_loop_pool& loops, ruvia::event_loop owner, runtime_state& runtime,
        node_log_buffer& logs)
        : loops_(loops),
          owner_(std::move(owner)),
          runtime_(runtime),
          logs_(logs),
          health_supervisor_(origin_health_supervisor::create(owner_, runtime_, health_, health_probe_tls_)),
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
        for (std::size_t index = 0; index < loops_.loop_count(); ++index) {
            workers_.push_back(std::make_unique<worker_state>(loops_.loop(index).executor(),
                logs_.worker_producer(index)));
        }
    }

    ~data_plane() {
        stop_on_owner();
    }

    void start() {
        health_supervisor_->request_start();
    }

    [[nodiscard]] bool healthy() const {
        return health_.all_healthy();
    }

    [[nodiscard]] std::vector<origin_health_registry::snapshot> origin_health_reports() const {
        return health_.reports();
    }

    [[nodiscard]] runtime_metrics_snapshot metrics() {
        runtime_metrics_snapshot result;
        for (const auto& worker : workers_) {
            const auto sample = worker->metrics_.sample();
            result.cpu_usage_ += sample.cpu_usage_;
            result.memory_usage_ += sample.memory_usage_;
            result.traffic_out_bps_ += sample.traffic_out_bps_;
            result.connection_count_ += sample.connection_count_;
            result.load1m_ += sample.load1m_;
        }
        if (!workers_.empty()) {
            result.cpu_usage_ /= static_cast<double>(workers_.size());
            result.memory_usage_ /= static_cast<double>(workers_.size());
            result.load1m_ /= static_cast<double>(workers_.size());
        }
        return result;
    }

    // Distinct client IP addresses with an open connection on any worker, gathered from each
    // worker's own registry (count_connected_ips). Runs on the owner loop.
    [[nodiscard]] ruvia::task<std::optional<std::int64_t>> connected_ip_count(ruvia::stop_token stop) {
        if (!owner_.is_current()) {
            throw std::logic_error("connected client count must run on the data plane owner");
        }
        std::vector<worker_connected_clients> workers;
        workers.reserve(workers_.size());
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            workers.push_back({.loop_ = loops_.loop(index), .clients_ = &workers_[index]->clients_});
        }
        co_return co_await count_connected_ips(owner_.handle(), workers, std::move(stop));
    }

    [[nodiscard]] bool drained() const noexcept {
        return std::ranges::all_of(
            workers_, [](const auto& worker) { return worker->metrics_.connection_count() == 0; });
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
        reload.activation_gate_ = std::make_shared<listener_activation_gate>();
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
                        key, http_listener::create(
                                 loops_.loop(worker_index), runtime_, health_,
                                 workers_[worker_index]->metrics_, workers_[worker_index]->clients_,
                                 workers_[worker_index]->logs_,
                                 workers_[worker_index]->origin_connections_, request_buffers_,
                                 response_buffers_,
                                 asio::ip::tcp::endpoint(
                                     bind_address(endpoint.ip_address()),
                                     static_cast<std::uint16_t>(endpoint.http_port()))));
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
                        https_key, https_listener::create(
                                       loops_.loop(worker_index), runtime_, health_, tls_contexts_,
                                       workers_[worker_index]->metrics_,
                                       workers_[worker_index]->clients_,
                                       workers_[worker_index]->logs_,
                                       workers_[worker_index]->origin_connections_, request_buffers_,
                                       response_buffers_,
                                       asio::ip::tcp::endpoint(
                                           bind_address(endpoint.ip_address()),
                                           static_cast<std::uint16_t>(endpoint.https_port()))));
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
                listener->request_start(reload.activation_gate_);
            }
            for (const auto& [_, listener] : reload.https_additions_) {
                listener->request_start(reload.activation_gate_);
            }
            reload.primed_ = true;
        } catch (...) {
            abort(reload);
            throw;
        }
    }

    void activate(prepared_data_plane_reload reload) noexcept {
        health_.publish(*reload.health_states_);
        tls_contexts_.publish(std::move(reload.tls_contexts_));
        runtime_.publish(std::move(reload.config_));
        listeners_.swap(reload.listeners_);
        https_listeners_.swap(reload.https_listeners_);
        reload.activation_gate_->activate();
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
        worker_state(asio::any_io_executor executor, node_log_buffer::producer producer)
            : origin_connections_(std::move(executor), origin_tls_),
              logs_(producer) {}

        runtime_metrics metrics_;
        connected_clients clients_;
        origin_tls_context origin_tls_;
        origin_connection_pool origin_connections_;
        node_log_buffer::producer logs_;
    };

    [[nodiscard]] prepared_data_plane_reload finish_prepare(prepared_data_plane_reload reload) const {
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
        origin_health_registry::key_set origin_keys;
        for (const auto* website : reload.config_->websites()) {
            for (const auto& origin : website->origins()) {
                origin_keys.emplace(origin_health_registry::get_key(website->id(), origin.id()));
            }
        }
        reload.health_states_ = health_.prepare(origin_keys);
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
    node_log_buffer& logs_;
    origin_health_registry health_;
    origin_tls_context health_probe_tls_;
    tls_context_registry tls_contexts_;
    std::shared_ptr<origin_health_supervisor> health_supervisor_;
    buffered_bytes_budget request_buffers_{maximum_request_buffer_bytes};
    buffered_bytes_budget response_buffers_{maximum_response_buffer_bytes};
    ruvia::event_loop_stop_registration stop_registration_;
    std::vector<std::unique_ptr<worker_state>> workers_;
    std::unordered_map<std::string, std::shared_ptr<http_listener>> listeners_;
    std::unordered_map<std::string, std::shared_ptr<https_listener>> https_listeners_;
};

}  // namespace flexedge::node
