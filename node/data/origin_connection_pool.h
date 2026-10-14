#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

// Each data-plane worker owns its pool, so an acquired transport always remains
// on the event loop that created its resolver, socket, and TLS stream.
class origin_connection_pool final {
public:
    origin_connection_pool(asio::any_io_executor executor, origin_tls_context& tls_context)
        : executor_(std::move(executor)),
          tls_context_(tls_context) {}

    origin_connection_pool(const origin_connection_pool&) = delete;
    origin_connection_pool& operator=(const origin_connection_pool&) = delete;

    [[nodiscard]] std::unique_ptr<origin_transport> acquire(const origin_connection_key& key) {
        const auto now = std::chrono::steady_clock::now();
        for (auto iterator = idle_.begin(); iterator != idle_.end();) {
            if (now - iterator->released_at_ > max_idle_age) {
                iterator->transport_->close();
                iterator = idle_.erase(iterator);
                continue;
            }
            if (iterator->key_ == key) {
                auto transport = std::move(iterator->transport_);
                idle_.erase(iterator);
                return transport;
            }
            ++iterator;
        }
        return std::make_unique<origin_transport>(executor_, tls_context_);
    }

    void release(origin_connection_key key, std::unique_ptr<origin_transport> transport) {
        if (!transport || !transport->connected()) {
            return;
        }
        if (idle_.size() == max_idle_connections) {
            idle_.front().transport_->close();
            idle_.erase(idle_.begin());
        }
        idle_.push_back({.key_ = std::move(key),
            .transport_ = std::move(transport),
            .released_at_ = std::chrono::steady_clock::now()});
    }

    void close() noexcept {
        for (auto& idle : idle_) {
            idle.transport_->close();
        }
        idle_.clear();
    }

private:
    struct idle_connection final {
        origin_connection_key key_;
        std::unique_ptr<origin_transport> transport_;
        std::chrono::steady_clock::time_point released_at_;
    };

    static constexpr std::size_t max_idle_connections{64};
    static constexpr auto max_idle_age = std::chrono::seconds(30);

    asio::any_io_executor executor_;
    origin_tls_context& tls_context_;
    std::vector<idle_connection> idle_;
};

}  // namespace flexedge::node
