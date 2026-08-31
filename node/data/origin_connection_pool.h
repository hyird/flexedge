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

struct OriginConnectionKey final {
    std::string host;
    std::uint16_t port{};
    bool secure{};

    [[nodiscard]] bool operator==(const OriginConnectionKey&) const noexcept = default;
};

// Each data-plane worker owns its pool, so an acquired transport always remains
// on the event loop that created its resolver, socket, and TLS stream.
class OriginConnectionPool final {
  public:
    OriginConnectionPool(asio::any_io_executor executor, OriginTlsContext& tlsContext)
        : executor_(std::move(executor)), tlsContext_(tlsContext) {}

    OriginConnectionPool(const OriginConnectionPool&) = delete;
    OriginConnectionPool& operator=(const OriginConnectionPool&) = delete;

    [[nodiscard]] std::unique_ptr<OriginTransport> acquire(const OriginConnectionKey& key) {
        const auto now = std::chrono::steady_clock::now();
        for (auto iterator = idle_.begin(); iterator != idle_.end();) {
            if (now - iterator->releasedAt > kMaxIdleAge) {
                iterator->transport->close();
                iterator = idle_.erase(iterator);
                continue;
            }
            if (iterator->key == key) {
                auto transport = std::move(iterator->transport);
                idle_.erase(iterator);
                return transport;
            }
            ++iterator;
        }
        return std::make_unique<OriginTransport>(executor_, tlsContext_);
    }

    void release(OriginConnectionKey key, std::unique_ptr<OriginTransport> transport) {
        if (!transport || !transport->connected()) {
            return;
        }
        if (idle_.size() == kMaxIdleConnections) {
            idle_.front().transport->close();
            idle_.erase(idle_.begin());
        }
        idle_.push_back({.key = std::move(key),
                         .transport = std::move(transport),
                         .releasedAt = std::chrono::steady_clock::now()});
    }

    void close() noexcept {
        for (auto& idle : idle_) {
            idle.transport->close();
        }
        idle_.clear();
    }

  private:
    struct IdleConnection final {
        OriginConnectionKey key;
        std::unique_ptr<OriginTransport> transport;
        std::chrono::steady_clock::time_point releasedAt;
    };

    static constexpr std::size_t kMaxIdleConnections{64};
    static constexpr auto kMaxIdleAge = std::chrono::seconds(30);

    asio::any_io_executor executor_;
    OriginTlsContext& tlsContext_;
    std::vector<IdleConnection> idle_;
};

} // namespace flexedge::node
