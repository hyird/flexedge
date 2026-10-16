#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <asio/ip/address.hpp>

namespace flexedge::node {

// Open data-plane connections per client IP address on one worker. Only the owning worker loop
// opens, closes and copies it; other loops receive immutable data_worker snapshots.
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

}  // namespace flexedge::node
