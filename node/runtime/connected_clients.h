#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/worker_handle.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

namespace flexedge::node {

// Open data-plane connections per client IP address on one worker. Only the owning worker loop
// opens, closes and copies it; other loops receive immutable copies (count_connected_ips).
class connected_clients final {
public:
    // IPv6 form of the address: an IPv4 client and its IPv4-mapped spelling on a dual-stack
    // listener share one key. IPv6 scope ids are not part of the client identity.
    using address_key = std::array<unsigned char, 16>;

    [[nodiscard]] static address_key get_key(const asio::ip::address& address) noexcept {
        if (address.is_v4()) {
            return asio::ip::make_address_v6(asio::ip::v4_mapped, address.to_v4()).to_bytes();
        }
        return address.to_v6().to_bytes();
    }

    void opened(const address_key& address) {
        ++connections_[address];
    }

    void closed(const address_key& address) noexcept {
        const auto found = connections_.find(address);
        if (found != connections_.end() && --found->second == 0) {
            connections_.erase(found);
        }
    }

    [[nodiscard]] std::vector<address_key> snapshot() const {
        std::vector<address_key> addresses;
        addresses.reserve(connections_.size());
        for (const auto& [address, _] : connections_) {
            addresses.push_back(address);
        }
        return addresses;
    }

private:
    struct address_hash final {
        [[nodiscard]] std::size_t operator()(const address_key& address) const noexcept {
            std::uint64_t hash{14695981039346656037ULL};
            for (const auto byte : address) {
                hash = (hash ^ byte) * 1099511628211ULL;
            }
            return static_cast<std::size_t>(hash);
        }
    };

    std::unordered_map<address_key, std::uint32_t, address_hash> connections_;
};

// One data-plane worker loop and the registry it owns.
struct worker_connected_clients final {
    ruvia::event_loop loop_;
    const connected_clients* clients_{};
};

inline constexpr auto connected_clients_snapshot_timeout = std::chrono::seconds(2);

// Distinct client IP addresses with an open connection on any of the workers; runs on `owner`.
// SO_REUSEPORT spreads one client's connections across workers, so per-worker counts cannot be
// added. Each worker copies its own address set on its loop and sends the copy here; the union
// is counted. Empty when a worker cannot answer, so callers omit the value instead of
// reporting a partial count.
inline ruvia::task<std::optional<std::int64_t>> count_connected_ips(ruvia::worker_handle owner,
    std::span<const worker_connected_clients> workers, ruvia::stop_token stop) {
    if (workers.empty()) {
        co_return 0;
    }
    using address_set = std::vector<connected_clients::address_key>;
    auto [sender, receiver] =
        ruvia::make_channel<address_set>(std::move(owner), {.capacity_ = workers.size()});
    for (const auto& worker : workers) {
        const auto posted = worker.loop_.post([sender, clients = worker.clients_] {
            try {
                (void)sender.send(clients->snapshot());
            } catch (...) {
                // Without this worker's copy no count is reported; closing ends the wait now.
                sender.close();
            }
        });
        if (!posted.accepted()) {
            co_return std::nullopt;
        }
    }
    address_set addresses;
    for (std::size_t received = 0; received < workers.size(); ++received) {
        auto copy = co_await receiver.receive_for(connected_clients_snapshot_timeout, stop);
        if (!copy.has_value()) {
            co_return std::nullopt;
        }
        const auto& worker_addresses = copy.value();
        addresses.insert(addresses.end(), worker_addresses.begin(), worker_addresses.end());
    }
    std::ranges::sort(addresses);
    const auto duplicates = std::ranges::unique(addresses);
    addresses.erase(duplicates.begin(), duplicates.end());
    co_return static_cast<std::int64_t>(addresses.size());
}

}  // namespace flexedge::node
