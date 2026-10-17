#pragma once

#include <ruvia/core/timer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/any_io_executor.hpp>

#include "node/data/origin_transport.h"

namespace flexedge::node {

struct origin_connection_key final {
    std::string host_;
    std::uint16_t port_{};
    bool secure_{};

    [[nodiscard]] bool operator==(const origin_connection_key&) const noexcept = default;
};

class origin_connection_pool;
struct origin_connection_deleter final {
    origin_connection_pool* owner_{};
    void operator()(origin_transport* transport) const noexcept;
};
using pooled_origin_connection = std::unique_ptr<origin_transport, origin_connection_deleter>;

// Each data-plane worker owns its pool, so an acquired transport always remains
// on the event loop that created its resolver, socket, and TLS stream.
class origin_connection_pool final {
public:
    origin_connection_pool(asio::any_io_executor executor, origin_tls_context& tls_context)
        : executor_(std::move(executor)),
          tls_context_(tls_context) {}

    origin_connection_pool(const origin_connection_pool&) = delete;
    origin_connection_pool& operator=(const origin_connection_pool&) = delete;

    [[nodiscard]] pooled_origin_connection acquire(const origin_connection_key& key) {
        const auto now = std::chrono::steady_clock::now();
        expire_idle(now);
        for (auto iterator = idle_.rbegin(); iterator != idle_.rend(); ++iterator) {
            if (iterator->key_ == key) {
                auto transport = std::move(iterator->transport_);
                idle_.erase((iterator + 1).base());
                tracked_.at(transport.get()).idle_ = false;
                return transport;
            }
        }
        pooled_origin_connection transport(new origin_transport(executor_, tls_context_), {this});
        tracked_.emplace(transport.get(), tracked_connection{.key_ = key});
        return transport;
    }

    void release(origin_connection_key key, pooled_origin_connection transport) {
        if (!transport || !transport->connected()) {
            return;
        }
        if (idle_.size() == max_idle_connections) {
            idle_.front().transport_->close();
            idle_.erase(idle_.begin());
        }
        tracked_.at(transport.get()).idle_ = true;
        idle_.push_back({.key_ = std::move(key),
            .transport_ = std::move(transport),
            .released_at_ = std::chrono::steady_clock::now()});
    }

    ruvia::task<void> watch_dns(ruvia::event_loop loop, ruvia::stop_token stop,
        std::chrono::steady_clock::duration interval = dns_check_interval) {
        const auto worker = loop.handle();
        while (!stop.stop_requested()) {
            if (co_await ruvia::sleep_for(worker, interval, stop) == ruvia::timer_sleep_result::stop_requested) {
                co_return;
            }
            expire_idle(std::chrono::steady_clock::now());
            std::vector<origin_connection_key> keys;
            for (const auto& [transport, entry] : tracked_) {
                if (transport->connected()) {
                    keys.push_back(entry.key_);
                }
            }
            std::ranges::sort(keys, [](const auto& left, const auto& right) {
                return std::tie(left.host_, left.port_, left.secure_) < std::tie(right.host_, right.port_, right.secure_);
            });
            keys.erase(std::ranges::unique(keys).begin(), keys.end());
            for (const auto& key : keys) {
                const auto addresses = co_await resolve_dns(loop, key.host_, key.port_, stop);
                if (!addresses) {
                    continue;
                }
                // Re-read live membership after the lookup: requests may finish while it
                // is suspended. Raw transports never escape this owning worker.
                for (const auto& [transport, entry] : tracked_) {
                    if (entry.key_ == key && transport->connected() && transport->addresses() != *addresses) {
                        transport->invalidate_dns();
                        if (entry.idle_) {
                            transport->close();
                        }
                    }
                }
            }
        }
    }

    void close() noexcept {
        for (auto& idle : idle_) {
            idle.transport_->close();
        }
        idle_.clear();
    }

private:
    void expire_idle(std::chrono::steady_clock::time_point now) {
        std::erase_if(idle_, [now](const auto& entry) { return now - entry.released_at_ > max_idle_age; });
    }
    friend struct origin_connection_deleter;
    struct tracked_connection final {
        origin_connection_key key_;
        bool idle_{};
    };
    struct idle_connection final {
        origin_connection_key key_;
        pooled_origin_connection transport_;
        std::chrono::steady_clock::time_point released_at_;
    };

    static constexpr std::size_t max_idle_connections{64};
    static constexpr auto max_idle_age = std::chrono::seconds(30);

    asio::any_io_executor executor_;
    origin_tls_context& tls_context_;
    std::unordered_map<origin_transport*, tracked_connection> tracked_;
    std::vector<idle_connection> idle_;
};

inline void origin_connection_deleter::operator()(origin_transport* transport) const noexcept {
    owner_->tracked_.erase(transport);
    delete transport;
}

}  // namespace flexedge::node
