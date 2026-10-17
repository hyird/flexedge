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
#include <variant>
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
    retired,
    cache_command };

// Cross-loop messages contain owned immutable business values, never runtime objects,
// listener handles, completion callbacks or channels back into another worker.
struct data_worker_request final {
    data_worker_operation operation_{};
    std::uint64_t sequence_{};
    std::uint64_t committed_sequence_{};
    std::shared_ptr<const compiled_config> config_{};
    std::shared_ptr<const v2::CacheCommand> cache_command_{};
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
    std::shared_ptr<const v2::CacheResult> cache_result_{};
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
        ruvia::channel_sender<owned_log_batch> logs, std::size_t worker_count,
        std::shared_ptr<response_cache_publication> cache,
        ruvia::channel_sender<owned_data_worker_result> cache_results,
        inherited_listeners inherited = {})
        : loop_(std::move(loop)),
          worker_(loop_.handle()),
          identity_(identity),
          requests_(std::move(requests)),
          results_(std::move(results)),
          cache_results_(std::move(cache_results)),
          cache_(std::move(cache)),
          operations_(worker_),
          origin_connections_(loop_.executor(), origin_tls_),
          logs_(loop_, std::move(logs)),
          health_(origin_health_supervisor::create(loop_, runtime_, origin_tls_)),
          request_buffers_(maximum_request_buffer_bytes / worker_count),
          response_buffers_(maximum_response_buffer_bytes / worker_count),
          inherited_(std::move(inherited)),
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
              if (preserve_listeners_) {
                  inherited_.release_to_process();
              } else {
                  inherited_.close_unused();
              }
              co_await health_->stop_and_wait();
              origin_connections_.close();
              reload_.reset();
              tls_contexts_.publish({});
              runtime_.publish_execution({});
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
    void request_stop_accepting(bool preserve_listeners = false) noexcept {
        try {
            (void)loop_.defer_cleanup([this, preserve_listeners] { stop_accepting(preserve_listeners); });
        } catch (...) {
            // The loop's stop hook repeats cleanup when post admission has closed.
        }
    }

private:
    using listener_map = std::unordered_map<std::string, std::shared_ptr<edge_listener>>;

    struct retained_resources final {};

    struct replacement_resources final {
        std::shared_ptr<tls_context_set> tls_contexts_;
        origin_health_state::prepared health_;
        listener_map listeners_{};
        std::vector<std::shared_ptr<edge_listener>> additions_{};
        std::vector<std::shared_ptr<edge_listener>> retired_{};
    };

    // Before the vote these values are the candidate; afterwards the same slots own the
    // displaced runtime. A reconnect has no replacement resources and cannot reset TLS
    // state, health streaks or in-flight probes.
    struct reload_participant final {
        enum class phase { prepared,
            tentative };

        ~reload_participant() {
            if (phase_ == phase::prepared) {
                if (const auto* replacement = std::get_if<replacement_resources>(&resources_)) {
                    for (const auto& listener : replacement->additions_) {
                        listener->request_stop();
                    }
                }
            }
        }

        std::uint64_t sequence_{};
        std::shared_ptr<const worker_execution_view> execution_{};
        std::variant<retained_resources, replacement_resources> resources_{};
        phase phase_{phase::prepared};
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
                    if (reload_ && reload_->phase_ == reload_participant::phase::tentative &&
                        reload_->sequence_ <= request->committed_sequence_) {
                        finalize(reload_->sequence_);
                    }
                    switch (request->operation_) {
                        case data_worker_operation::cache_command:
                            operations_.spawn(execute_cache(request->sequence_, request->cache_command_));
                            continue;
                        case data_worker_operation::prepare:
                            prepare(*request);
                            if (const auto* replacement = std::get_if<replacement_resources>(&reload_->resources_)) {
                                result.added_listeners_ = replacement->additions_.size();
                                result.retired_listeners_ = replacement->retired_.size();
                            }
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
        return {.runtime_ = runtime_, .health_ = *health_, .metrics_ = metrics_, .clients_ = clients_, .logs_ = logs_, .origin_connections_ = origin_connections_, .request_buffers_ = request_buffers_, .response_buffers_ = response_buffers_, .cache_ = &cache_, .refresh_ = &cache_refresh_};
    }

    ruvia::task<std::pair<bool, std::string>> prewarm_url(const v2::CacheCommand& command,
        const std::string& value) {
        const auto url = flexedge::cache::parse_url(value);
        const auto config = runtime_.config();
        const auto* site = url && config ? config->website(url->hostname_) : nullptr;
        if (!site || std::ranges::find(command.website_ids(), site->id()) == command.website_ids().end() ||
            !site->has_response_cache() || site->response_cache().id() != command.policy_id()) {
            co_return std::pair{false, std::string("网站缓存策略已改变或 URL 不属于该网站")};
        }
        auto [completed, completion] = ruvia::make_channel<std::pair<bool, std::string>>(worker_, {.capacity_ = 1});
        auto [retired, retirement] = ruvia::make_channel<bool>(worker_, {.capacity_ = 1});
        auto connection = std::make_shared<edge_connection>([retired] { (void)retired.send(true); });
        auto session = std::make_shared<basic_http_session<asio::ip::tcp::socket>>(
            asio::ip::tcp::socket(loop_.executor()), session_context(), ++prewarm_sequence_,
            std::move(connection), url->scheme_ == "https", std::string{}, true);
        session->prewarm("GET " + url->target_ + " HTTP/1.1\r\nHost: " + url->authority_ +
                             "\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
            [completed](bool ok, std::string error) { (void)completed.send({ok, std::move(error)}); });
        const auto result = co_await completion.receive_for(std::chrono::seconds(15), operations_.get_stop_token());
        session->close();
        session.reset();
        // All callbacks retaining the synthetic session must retire on this loop before
        // its worker-owned dependencies can be destroyed.
        (void)co_await retirement.receive();
        if (!result.has_value()) {
            co_return std::pair{false, std::string("预热超时或节点正在停止")};
        }
        co_return result.value();
    }

    ruvia::task<void> execute_cache(std::uint64_t sequence, std::shared_ptr<const v2::CacheCommand> command) {
        if (!command) {
            co_return;
        }
        auto result = cache_.command_result(command->attempt_id());
        if (!result && cache_.claim_command(command->attempt_id())) {
            v2::CacheResult value;
            value.set_task_id(command->task_id());
            value.set_generation(command->generation());
            value.set_attempt_id(command->attempt_id());
            value.set_ok(true);
            try {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                if (command->expires_unix_ms() <= now) {
                    throw std::runtime_error("缓存操作已过期");
                }
                if (command->operation() == "prewarm") {
                    for (const auto& url : command->urls()) {
                        if (operations_.stop_requested()) {
                            throw std::runtime_error("节点正在停止");
                        }
                        const auto clock = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                                               .count();
                        auto outcome = clock >= command->expires_unix_ms()
                                           ? std::pair{false, std::string("缓存操作已过期")}
                                           : co_await prewarm_url(*command, url);
                        if (outcome.first) {
                            value.set_completed_urls(value.completed_urls() + 1);
                        } else {
                            value.set_ok(false);
                            value.add_failures("第 " + std::to_string(value.completed_urls() + value.failures_size() + 1) + " 个 URL：" + outcome.second);
                        }
                    }
                    if (!value.ok()) {
                        value.set_error("部分 URL 未能写入缓存");
                    }
                } else if (command->operation() == "refresh" || command->operation() == "clean") {
                    std::vector<cache_invalidation_url> urls;
                    if (command->scope() == "urls" || command->scope() == "prefix") {
                        const auto config = runtime_.config();
                        for (const auto& address : command->urls()) {
                            auto url = flexedge::cache::parse_url(address);
                            const auto* site = url && config ? config->website(url->hostname_) : nullptr;
                            if (!site || std::ranges::find(command->website_ids(), site->id()) == command->website_ids().end() ||
                                !site->has_response_cache() || site->response_cache().id() != command->policy_id()) {
                                throw std::runtime_error("网站缓存策略已改变或 URL 不属于该网站");
                            }
                            urls.push_back({site->id(), std::move(*url)});
                        }
                    }
                    const auto [objects, bytes] = cache_.invalidate(*command, urls);
                    value.set_affected_objects(objects);
                    value.set_affected_bytes(bytes);
                } else {
                    throw std::runtime_error("不支持的缓存操作");
                }
            } catch (const std::exception& error) {
                value.set_ok(false);
                value.set_error(error.what());
            }
            cache_.complete_command(value);
            result = cache_.command_result(command->attempt_id());
        }
        if (result) {
            (void)cache_results_.send(std::make_unique<const data_worker_result>(data_worker_result{
                .operation_ = data_worker_operation::cache_command, .sequence_ = sequence, .worker_id_ = identity_, .cache_result_ = std::move(result)}));
        }
    }

    void prepare(const data_worker_request& request) {
        require_accepting();
        require_current(request);
        if (reload_ && reload_->phase_ == reload_participant::phase::tentative) {
            throw std::logic_error("data worker reload decision is missing");
        }
        if (!request.config_) {
            throw std::invalid_argument("data worker config cannot be empty");
        }
        if (reload_) {
            if (request.sequence_ <= reload_->sequence_) {
                throw std::logic_error("stale data worker reload");
            }
            // Recover an abandoned prepare before admitting its replacement.
            reload_.reset();
        }
        runtime_.validate_next(*request.config_);
        auto reload = std::make_unique<reload_participant>();
        reload->sequence_ = request.sequence_;
        reload->execution_ = std::make_shared<const worker_execution_view>(request.config_);
        const auto active = runtime_.config();
        const auto unchanged = active &&
                               active->node_spec_digest() == request.config_->node_spec_digest() &&
                               active->release_digest() == request.config_->release_digest();
        if (!unchanged) {
            auto& replacement = reload->resources_.emplace<replacement_resources>(
                replacement_resources{std::make_shared<tls_context_set>(*request.config_),
                    health_->prepare(active.get(), *request.config_)});
            if (request.config_->enabled()) {
                for (const auto& endpoint : request.config_->endpoints()) {
                    prepare_listener(replacement, endpoint.ip_address(), endpoint.http_port(), false);
                    if (!replacement.tls_contexts_->empty()) {
                        prepare_listener(replacement, endpoint.ip_address(), endpoint.https_port(), true);
                    }
                }
            }
            for (const auto& [key, listener] : listeners_) {
                if (!replacement.listeners_.contains(key)) {
                    replacement.retired_.push_back(listener);
                }
            }
            for (const auto& listener : replacement.additions_) {
                listener->request_start();
            }
        }
        require_current(request);
        reload_ = std::move(reload);
    }

    void prepare_listener(replacement_resources& reload, std::string_view address, std::uint32_t port, bool secure) {
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
        const asio::ip::tcp::endpoint endpoint{bind, static_cast<std::uint16_t>(port)};
        auto listener = edge_listener::create(loop_, session_context(),
            endpoint, secure ? &tls_contexts_ : nullptr, inherited_.take(endpoint));
        owned_listeners_.push_back(listener);
        reload.additions_.push_back(listener);
        reload.listeners_.emplace(std::move(key), std::move(listener));
    }

    void exchange_runtime(reload_participant& reload) noexcept {
        if (auto* replacement = std::get_if<replacement_resources>(&reload.resources_)) {
            replacement->health_ = health_->activate(std::move(replacement->health_));
            auto previous_tls = tls_contexts_.current();
            tls_contexts_.publish(std::move(replacement->tls_contexts_));
            replacement->tls_contexts_ = std::move(previous_tls);
            listeners_.swap(replacement->listeners_);
        }
        auto previous = runtime_.execution();
        runtime_.publish_execution(std::move(reload.execution_));
        reload.execution_ = std::move(previous);
    }

    void activate(std::uint64_t sequence) {
        if (!reload_ || reload_->sequence_ != sequence ||
            reload_->phase_ != reload_participant::phase::prepared) {
            throw std::logic_error("data worker reload is not prepared");
        }
        exchange_runtime(*reload_);
        reload_->phase_ = reload_participant::phase::tentative;
        if (const auto* replacement = std::get_if<replacement_resources>(&reload_->resources_)) {
            for (const auto& listener : replacement->additions_) {
                listener->activate();
            }
        }
    }

    void finalize(std::uint64_t sequence) noexcept {
        if (reload_ && reload_->sequence_ == sequence &&
            reload_->phase_ == reload_participant::phase::tentative) {
            if (const auto* replacement = std::get_if<replacement_resources>(&reload_->resources_)) {
                for (const auto& listener : replacement->retired_) {
                    listener->request_stop();
                }
            }
            reload_.reset();
            inherited_.close_unused();
            settled_sequence_ = (std::max)(settled_sequence_, sequence);
        }
    }

    void rollback(std::uint64_t sequence) noexcept {
        if (sequence <= settled_sequence_) {
            return;
        }
        settled_sequence_ = sequence;
        if (!reload_ || reload_->sequence_ != sequence) {
            return;
        }
        if (reload_->phase_ == reload_participant::phase::tentative) {
            exchange_runtime(*reload_);
            if (const auto* replacement = std::get_if<replacement_resources>(&reload_->resources_)) {
                for (const auto& listener : replacement->additions_) {
                    listener->request_stop();
                }
            }
        }
        reload_.reset();
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

    void stop_accepting(bool preserve_listeners = false) noexcept {
        preserve_listeners_ = preserve_listeners_ || preserve_listeners;
        stopping_ = true;
        if (reload_ && reload_->phase_ == reload_participant::phase::prepared) {
            reload_.reset();
        }
        // Admission stop is not a transaction decision. The control loop may have just
        // committed globally; only its immutable decision can roll back or finalize.
        for (const auto& listener : owned_listeners_) {
            listener->request_stop(preserve_listeners_);
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
    ruvia::channel_sender<owned_data_worker_result> cache_results_;
    response_cache cache_;
    ruvia::task_scope operations_;
    cache_refresh_runtime cache_refresh_{loop_, operations_};
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
    inherited_listeners inherited_;
    listener_map listeners_;
    std::vector<std::shared_ptr<edge_listener>> owned_listeners_;
    std::unique_ptr<reload_participant> reload_;
    std::uint64_t settled_sequence_{};
    std::uint64_t prewarm_sequence_{};
    bool stopping_{};
    bool preserve_listeners_{};
    ruvia::event_loop_stop_registration stop_registration_;
};

}  // namespace flexedge::node
