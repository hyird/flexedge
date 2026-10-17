#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "node/data/data_worker.h"

namespace flexedge::node {

class config_activation;

struct data_plane_report final {
    std::optional<data_plane_load> load_{};
    std::shared_ptr<const origin_health_snapshot> origin_health_{};
    std::uint64_t cache_memory_bytes_{}, cache_disk_bytes_{};
};

// A retired worker or unconfirmed rollback makes this node unavailable. This failure
// must leave the control reconnect loop so process assembly joins every data worker.
class data_plane_unavailable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Control-loop FIFO. Each suspended operation owns its own channel waiter; a channel
// receiver itself only supports one pending receive. The lease covers a whole reload.
class data_plane_operations final {
public:
    class lease final {
    public:
        explicit lease(data_plane_operations& operations) noexcept
            : operations_(&operations) {}
        lease(lease&& other) noexcept
            : operations_(std::exchange(other.operations_, nullptr)) {}
        lease& operator=(lease&&) = delete;
        lease(const lease&) = delete;
        lease& operator=(const lease&) = delete;
        ~lease() {
            if (operations_) {
                operations_->release();
            }
        }

    private:
        data_plane_operations* operations_;
    };

    ruvia::task<lease> acquire(ruvia::worker_handle worker, std::chrono::steady_clock::duration timeout,
        ruvia::stop_token stop = {}) {
        if (stop.stop_requested()) {
            throw std::runtime_error("data plane operation was interrupted");
        }
        if (!busy_) {
            busy_ = true;
            co_return lease(*this);
        }
        auto [sender, receiver] = ruvia::make_channel<bool>(std::move(worker), {.capacity_ = 1});
        waiter waiting{*this, std::move(sender)};
        waiters_.push_back(&waiting);
        const auto result = co_await receiver.receive_for(timeout, std::move(stop));
        if (!result.has_value()) {
            throw std::runtime_error("data plane operation was interrupted or timed out");
        }
        waiting.transferred_ = true;
        co_return lease(*this);
    }

private:
    struct waiter final {
        ~waiter() {
            std::erase(operations_.waiters_, this);
            if (granted_ && !transferred_) {
                operations_.release();
            }
        }
        data_plane_operations& operations_;
        ruvia::channel_sender<bool> sender_;
        bool granted_{};
        bool transferred_{};
    };

    void release() noexcept {
        while (!waiters_.empty()) {
            auto* next = waiters_.front();
            waiters_.erase(waiters_.begin());
            try {
                if (next->sender_.send(true).accepted()) {
                    next->granted_ = true;
                    return;
                }
            } catch (...) {
            }
        }
        busy_ = false;
    }

    bool busy_{};
    std::vector<waiter*> waiters_;
};

// A coordinator-owned control-loop lease. Its destructor only discards an abandoned
// prepare; commit and compensation are explicit coordinator decisions.
class data_plane_reload final {
public:
    data_plane_reload(data_plane_reload&& other) noexcept
        : requests_(std::move(other.requests_)),
          config_(std::move(other.config_)),
          sequence_(other.sequence_),
          added_(other.added_),
          retired_(other.retired_),
          deadline_(other.deadline_),
          timeout_(other.timeout_),
          operation_(std::move(other.operation_)),
          armed_(std::exchange(other.armed_, false)) {}
    data_plane_reload& operator=(data_plane_reload&&) = delete;
    data_plane_reload(const data_plane_reload&) = delete;
    data_plane_reload& operator=(const data_plane_reload&) = delete;

    ~data_plane_reload() {
        if (armed_) {
            for (const auto& sender : requests_) {
                try {
                    (void)sender.send(std::make_unique<const data_worker_request>(data_worker_request{
                        .operation_ = data_worker_operation::discard, .sequence_ = sequence_}));
                } catch (...) {
                    // A stopping worker discards pending resources in its own stop hook.
                }
            }
        }
    }

    [[nodiscard]] std::size_t added_listeners() const noexcept {
        return added_;
    }
    [[nodiscard]] std::size_t retired_listeners() const noexcept {
        return retired_;
    }

private:
    friend class data_plane;
    friend class config_activation;
    data_plane_reload(std::vector<ruvia::channel_sender<owned_data_worker_request>> requests,
        std::shared_ptr<const compiled_config> config, std::uint64_t sequence,
        std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::duration timeout,
        data_plane_operations::lease operation)
        : requests_(std::move(requests)),
          config_(std::move(config)),
          sequence_(sequence),
          deadline_(deadline),
          timeout_(timeout),
          operation_(std::move(operation)) {}

    std::vector<ruvia::channel_sender<owned_data_worker_request>> requests_;
    std::shared_ptr<const compiled_config> config_;
    std::uint64_t sequence_{};
    std::size_t added_{};
    std::size_t retired_{};
    std::chrono::steady_clock::time_point deadline_;
    std::chrono::steady_clock::duration timeout_;
    data_plane_operations::lease operation_;
    bool armed_{true};
};

class data_plane final {
public:
    data_plane(ruvia::event_loop_pool& loops, ruvia::event_loop owner, runtime_state& runtime,
        ruvia::channel_sender<owned_log_batch> logs,
        std::filesystem::path cache_directory = "cache",
        std::vector<inherited_listeners> inherited = {})
        : owner_(std::move(owner)),
          runtime_(runtime),
          retired_workers_(loops.loop_count()),
          results_(ruvia::make_channel<owned_data_worker_result>(owner_.handle(), {.capacity_ = loops.loop_count() * 4})),
          cache_results_(ruvia::make_channel<owned_data_worker_result>(owner_.handle(), {.capacity_ = loops.loop_count() * 64})),
          cache_(std::make_shared<response_cache_publication>(std::move(cache_directory))),
          empty_health_(std::make_shared<const origin_health_snapshot>()),
          last_health_(empty_health_),
          stop_registration_(owner_.on_stop([this]() -> ruvia::task<void> {
              request_stop_accepting();
              co_return;
          })) {
        if (!inherited.empty() && inherited.size() != loops.loop_count()) {
            throw std::invalid_argument("inherited node listener worker count mismatch");
        }
        workers_.reserve(loops.loop_count());
        requests_.reserve(loops.loop_count());
        data_loops_.reserve(loops.loop_count());
        for (std::size_t index = 0; index < loops.loop_count(); ++index) {
            data_loops_.push_back(loops.loop(index));
            auto [sender, receiver] = ruvia::make_channel<owned_data_worker_request>(loops.loop(index).handle(), {.capacity_ = 32});
            workers_.push_back(std::make_unique<data_worker>(loops.loop(index), index, std::move(receiver), results_.first, logs, loops.loop_count(), cache_, cache_results_.first,
                inherited.empty() ? inherited_listeners{} : std::move(inherited[index])));
            requests_.push_back(std::move(sender));
        }
    }

    ruvia::task<v2::CacheResult> execute_cache(v2::CacheCommand command, ruvia::stop_token stop = {}) {
        require_owner();
        auto lease = co_await cache_operations_.acquire(owner_.handle(), std::chrono::minutes(11), stop);
        require_running();
        const auto sequence = ++cache_sequence_;
        const auto attempt = command.attempt_id();
        send({.operation_ = data_worker_operation::cache_command, .sequence_ = sequence, .cache_command_ = std::make_shared<const v2::CacheCommand>(std::move(command))});
        for (;;) {
            const auto received = co_await cache_results_.second.receive_for(std::chrono::minutes(11), stop);
            if (!received.has_value()) {
                throw std::runtime_error("缓存操作未返回结果");
            }
            const auto& value = received.value();
            if (value->cache_result_ && value->cache_result_->attempt_id() == attempt) {
                co_return *value->cache_result_;
            }
        }
    }

    // Assembly retains every worker until both loop pools have joined (loop_shutdown).
    void start() {
        std::lock_guard lock(start_mutex_);
        if (started_) {
            return;
        }
        started_ = true;
        for (const auto& worker : workers_) {
            worker->start();
        }
    }

private:
    friend class config_activation;

    [[nodiscard]] ruvia::task<data_plane_reload> begin_reload(std::shared_ptr<const compiled_config> config,
        std::chrono::steady_clock::duration timeout) {
        require_owner();
        if (!config) {
            throw std::invalid_argument("data plane config cannot be empty");
        }
        if (timeout <= std::chrono::steady_clock::duration::zero()) {
            throw std::invalid_argument("data plane reload timeout must be positive");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto operation = co_await operations_.acquire(owner_.handle(), timeout);
        require_running();
        runtime_.validate_next(*config);
        start();
        co_return data_plane_reload(requests_, std::move(config), ++sequence_, deadline, timeout, std::move(operation));
    }

    ruvia::task<void> prepare_workers(data_plane_reload& reload) {
        send({.operation_ = data_worker_operation::prepare, .sequence_ = reload.sequence_, .config_ = reload.config_, .deadline_ = reload.deadline_});
        const auto replies = co_await receive(data_worker_operation::prepare, reload.sequence_, reload.deadline_);
        for (const auto& reply : replies) {
            if (!reply->ok_) {
                throw std::runtime_error(reply->error_);
            }
            reload.added_ += reply->added_listeners_;
            reload.retired_ += reply->retired_listeners_;
        }
    }

    ruvia::task<void> activate_workers(data_plane_reload& reload) {
        require_owner();
        require_running();
        send({.operation_ = data_worker_operation::activate, .sequence_ = reload.sequence_, .deadline_ = reload.deadline_});
        const auto replies = co_await receive(data_worker_operation::activate, reload.sequence_, reload.deadline_);
        for (const auto& reply : replies) {
            if (!reply->ok_) {
                throw std::runtime_error(reply->error_);
            }
        }
        require_running();
        if (std::chrono::steady_clock::now() >= reload.deadline_) {
            throw std::runtime_error("data plane activation deadline expired");
        }
    }

    void commit_reload(data_plane_reload& reload) noexcept {
        // Commit linearizes here on the control loop. Admission stops after the final
        // running check cannot revoke a vote on another loop.
        committed_sequence_ = reload.sequence_;
        runtime_.publish_source(std::move(reload.config_));
        last_health_ = empty_health_;
        reload.armed_ = false;
        // All workers committed. Losing finalization only retains old resources until the
        // next transaction or stop; it cannot change the committed configuration.
        for (const auto& sender : requests_) {
            try {
                (void)sender.send(std::make_unique<const data_worker_request>(data_worker_request{
                    .operation_ = data_worker_operation::finalize, .sequence_ = reload.sequence_, .committed_sequence_ = committed_sequence_}));
            } catch (...) {
            }
        }
    }

public:
    [[nodiscard]] ruvia::task<data_plane_report> report(ruvia::stop_token stop) {
        require_owner();
        start();
        const auto sequence = ++sequence_;
        std::vector<owned_data_worker_result> replies;
        std::optional<data_plane_operations::lease> operation;
        try {
            operation.emplace(co_await operations_.acquire(owner_.handle(), worker_report_timeout, stop));
            send({.operation_ = data_worker_operation::report, .sequence_ = sequence});
            replies = co_await receive(data_worker_operation::report, sequence, std::chrono::steady_clock::now() + worker_report_timeout, std::move(stop));
        } catch (const data_plane_unavailable&) {
            request_stop_accepting();
            throw;
        } catch (const std::exception&) {
            co_return data_plane_report{.origin_health_ = last_health_};
        }
        data_plane_load load;
        std::vector<connected_clients::address_key> addresses;
        std::vector<std::shared_ptr<const origin_health_snapshot>> health;
        for (const auto& reply : replies) {
            if (!reply->ok_ || !reply->snapshot_ || reply->snapshot_->config_ != runtime_.config()) {
                co_return data_plane_report{.origin_health_ = last_health_};
            }
            const auto& snapshot = *reply->snapshot_;
            load.connection_count_ += snapshot.connection_count_;
            load.traffic_out_bytes_ += snapshot.traffic_out_bytes_;
            load.logs_.published_events_ += snapshot.logs_.published_events_;
            load.logs_.queued_events_ += snapshot.logs_.queued_events_;
            load.logs_.dropped_events_ += snapshot.logs_.dropped_events_;
            addresses.insert(addresses.end(), snapshot.addresses_.begin(), snapshot.addresses_.end());
            health.push_back(snapshot.health_);
        }
        std::ranges::sort(addresses);
        addresses.erase(std::ranges::unique(addresses).begin(), addresses.end());
        load.connected_ip_count_ = static_cast<std::int64_t>(addresses.size());
        last_health_ = origin_health_snapshot::merge(health);
        data_plane_report report{.load_ = load, .origin_health_ = last_health_};
        // Count the shared immutable root once, rather than once per worker.
        const auto cache = cache_->snapshot();
        for (const auto& [key, variants] : cache->entries_) {
            (void)key;
            for (const auto& item : variants) {
                report.cache_memory_bytes_ += item->bytes_ - (item->body_ ? 0 : item->body_bytes_);
                if (item->file_) {
                    report.cache_disk_bytes_ += item->bytes_;
                }
            }
        }
        co_return report;
    }

    [[nodiscard]] ruvia::task<bool> wait_drained(std::chrono::steady_clock::duration timeout) {
        require_owner();
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto sequence = ++sequence_;
        try {
            const auto operation = co_await operations_.acquire(owner_.handle(), timeout);
            timeout = deadline - std::chrono::steady_clock::now();
            send({.operation_ = data_worker_operation::drain, .sequence_ = sequence, .timeout_ = timeout});
            const auto replies = co_await receive(data_worker_operation::drain, sequence, deadline);
            co_return std::ranges::all_of(replies, [](const auto& reply) { return reply->ok_; });
        } catch (const std::exception&) {
            co_return false;
        }
    }

    // Any thread. Cancellation and cleanup take place on each receiving worker.
    void request_stop_accepting(bool preserve_listeners = false) noexcept {
        stopping_.store(true, std::memory_order_release);
        for (const auto& worker : workers_) {
            worker->request_stop_accepting(preserve_listeners);
        }
    }

private:
    // Keep the coordinator's lease until durable restoration has also finished.
    ruvia::task<void> rollback_workers(data_plane_reload& reload) {
        require_owner();
        if (!reload.armed_) {
            co_return;
        }
        try {
            try {
                send({.operation_ = data_worker_operation::rollback, .sequence_ = reload.sequence_}, true);
            } catch (...) {
                // Still await every survivor. A sender closing precedes its owner's
                // retirement acknowledgement; one rejection cannot skip other replies.
            }
            const auto replies = co_await receive(data_worker_operation::rollback, reload.sequence_,
                std::chrono::steady_clock::now() + reload.timeout_);
            if (std::ranges::any_of(replies, [](const auto& reply) { return !reply->ok_; })) {
                throw std::runtime_error("data worker rollback failed");
            }
            reload.armed_ = false;
        } catch (...) {
            request_stop_accepting();
            reload.armed_ = false;
            throw data_plane_unavailable("data plane rollback could not be confirmed");
        }
        if (std::ranges::any_of(retired_workers_, [](bool retired) { return retired; })) {
            request_stop_accepting();
            throw data_plane_unavailable("data worker retired; surviving workers rolled back");
        }
    }

private:
    void require_running() const {
        if (stopping_.load(std::memory_order_acquire)) {
            throw std::runtime_error("data plane is stopping");
        }
        // These are assembly lifecycle capabilities, never business runtime shared with
        // another worker. Fence votes already queued before a data loop stopped admission.
        if (std::ranges::any_of(data_loops_, [](const auto& loop) { return !loop.accepting(); })) {
            throw data_plane_unavailable("data worker is retiring");
        }
    }

    void require_owner() const {
        if (!owner_.is_current()) {
            throw std::logic_error("data plane operation must run on its control loop");
        }
    }

    void send(data_worker_request request, bool allow_retired = false) {
        request.committed_sequence_ = committed_sequence_;
        std::exception_ptr failure;
        for (std::size_t index = 0; index != requests_.size(); ++index) {
            if (retired_workers_[index]) {
                if (!allow_retired && !failure) {
                    failure = std::make_exception_ptr(data_plane_unavailable("data worker retired"));
                }
                continue;
            }
            const auto& sender = requests_[index];
            try {
                if (!sender.send(std::make_unique<const data_worker_request>(request)).accepted()) {
                    throw std::runtime_error("data worker does not accept requests");
                }
            } catch (...) {
                if (!failure) {
                    failure = std::current_exception();
                }
            }
        }
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    ruvia::task<std::vector<owned_data_worker_result>> receive(data_worker_operation operation,
        std::uint64_t sequence, std::chrono::steady_clock::time_point deadline, ruvia::stop_token stop = {}) {
        std::vector<bool> seen(requests_.size());
        std::vector<owned_data_worker_result> replies;
        replies.reserve(requests_.size());
        const auto complete = [&] {
            for (std::size_t index = 0; index != requests_.size(); ++index) {
                if (!seen[index] && !retired_workers_[index]) {
                    return false;
                }
            }
            return true;
        };
        while (!complete()) {
            if (operation != data_worker_operation::rollback && std::ranges::any_of(retired_workers_, [](bool retired) { return retired; })) {
                throw data_plane_unavailable("data worker retired");
            }
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                throw std::runtime_error("data worker result timed out");
            }
            auto next = co_await results_.second.receive_for(remaining, stop);
            if (!next.has_value()) {
                if (next.status() == ruvia::worker_wait_status::timed_out) {
                    throw std::runtime_error("data worker result timed out");
                }
                throw std::runtime_error("data worker did not return a result");
            }
            auto reply = std::move(next).take_value();
            if (reply->worker_id_ >= seen.size()) {
                throw std::runtime_error("invalid data worker result identity");
            }
            if (reply->operation_ == data_worker_operation::retired) {
                retired_workers_[reply->worker_id_] = true;
                if (operation != data_worker_operation::rollback) {
                    throw data_plane_unavailable("data worker retired");
                }
                continue;
            }
            if (reply->sequence_ != sequence || reply->operation_ != operation) {
                continue;
            }
            if (seen[reply->worker_id_]) {
                throw std::runtime_error("invalid data worker result identity");
            }
            seen[reply->worker_id_] = true;
            replies.push_back(std::move(reply));
        }
        co_return replies;
    }

    ruvia::event_loop owner_;
    runtime_state& runtime_;
    std::vector<bool> retired_workers_;
    data_plane_operations operations_;
    data_plane_operations cache_operations_;
    std::uint64_t cache_sequence_{};
    std::mutex start_mutex_;
    bool started_{};
    std::atomic<bool> stopping_{};
    std::uint64_t sequence_{};
    std::uint64_t committed_sequence_{};
    std::pair<ruvia::channel_sender<owned_data_worker_result>, ruvia::channel_receiver<owned_data_worker_result>> results_;
    std::pair<ruvia::channel_sender<owned_data_worker_result>, ruvia::channel_receiver<owned_data_worker_result>> cache_results_;
    std::shared_ptr<response_cache_publication> cache_;
    std::shared_ptr<const origin_health_snapshot> empty_health_;
    std::shared_ptr<const origin_health_snapshot> last_health_;
    std::vector<std::unique_ptr<data_worker>> workers_;
    std::vector<ruvia::event_loop> data_loops_;
    std::vector<ruvia::channel_sender<owned_data_worker_request>> requests_;
    ruvia::event_loop_stop_registration stop_registration_;
};

}  // namespace flexedge::node
