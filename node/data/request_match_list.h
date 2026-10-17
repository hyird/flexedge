#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <asio/ip/address.hpp>

namespace flexedge::node {
struct request_list_value final {
    std::string value_{};
    std::uint64_t expires_unix_ms_{};
};

// Sorted, merged address ranges give logarithmic membership checks for IP/CIDR lists.
class request_match_list final {
public:
    request_match_list(std::string type, const std::vector<std::string>& entries)
        : request_match_list(std::move(type), permanent_values(entries)) {}
    request_match_list(std::string type, const std::vector<request_list_value>& entries)
        : type_(std::move(type)) {
        for (const auto& source : entries) {
            const auto& item = source.value_;
            has_expiration_ = has_expiration_ || source.expires_unix_ms_ != 0;
            const auto expires = source.expires_unix_ms_ ? source.expires_unix_ms_ : (std::numeric_limits<std::uint64_t>::max)();
            if (type_ == "ip") {
                add_network(item, expires);
            } else {
                if (item.empty() || item.size() > 2048) {
                    throw std::invalid_argument("invalid security list entry");
                }
                if (type_ == "country" && (item.size() != 2 || item[0] < 'A' || item[0] > 'Z' || item[1] < 'A' || item[1] > 'Z')) {
                    throw std::invalid_argument("invalid country code");
                }
                if (type_ == "asn") {
                    std::uint32_t value{};
                    const auto parsed = std::from_chars(item.data(), item.data() + item.size(), value);
                    if (parsed.ec != std::errc{} || parsed.ptr != item.data() + item.size() || !value || std::to_string(value) != item) {
                        throw std::invalid_argument("invalid ASN");
                    }
                }
                auto [found, inserted] = strings_.try_emplace(item, expires);
                if (!inserted) {
                    found->second = (std::max)(found->second, expires);
                }
            }
        }
        if (type_ != "ip" && type_ != "asn" && type_ != "country" && type_ != "user_agent") {
            throw std::invalid_argument("invalid security list type");
        }
        for (auto* ranges : {&ipv4_, &ipv6_}) {
            std::ranges::sort(*ranges, {}, &range::first_);
            std::vector<range> merged;
            for (const auto& item : *ranges) {
                if (!merged.empty() && item.expires_ == merged.back().expires_ && item.first_ <= merged.back().last_) {
                    merged.back().last_ = (std::max)(merged.back().last_, item.last_);
                } else {
                    merged.push_back(item);
                }
            }
            *ranges = std::move(merged);
        }
        ipv4_tree_.reserve(ipv4_.size());
        ipv6_tree_.reserve(ipv6_.size());
        build_tree(ipv4_, ipv4_tree_, 0, ipv4_.size());
        build_tree(ipv6_, ipv6_tree_, 0, ipv6_.size());
        ipv4_.clear();
        ipv4_.shrink_to_fit();
        ipv6_.clear();
        ipv6_.shrink_to_fit();
    }
    [[nodiscard]] const std::string& type() const noexcept {
        return type_;
    }
    [[nodiscard]] bool contains(std::string_view input) const {
        const auto now = has_expiration_ ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()) : 0;
        if (type_ != "ip") {
            const auto found = strings_.find(std::string(input));
            return found != strings_.end() && found->second > now;
        }
        std::error_code error;
        auto address = asio::ip::make_address(input, error);
        if (error || (address.is_v6() && address.to_v6().scope_id())) {
            return false;
        }
        if (address.is_v6() && address.to_v6().is_v4_mapped()) {
            address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
        }
        const auto bytes = key(address);
        const auto& tree = address.is_v4() ? ipv4_tree_ : ipv6_tree_;
        return !tree.empty() && in_tree(tree, 0, bytes, now);
    }

private:
    using bytes = std::array<std::uint8_t, 16>;
    struct range final {
        bytes first_{}, last_{};
        std::uint64_t expires_{};
    };
    static constexpr auto no_node = (std::numeric_limits<std::size_t>::max)();
    struct tree_node final {
        range value_{};
        bytes maximum_end_{};
        std::uint64_t maximum_expiry_{};
        std::size_t left_{no_node}, right_{no_node};
    };
    static std::vector<request_list_value> permanent_values(const std::vector<std::string>& entries) {
        std::vector<request_list_value> result;
        result.reserve(entries.size());
        for (const auto& item : entries) {
            result.push_back({item});
        }
        return result;
    }
    static std::size_t build_tree(const std::vector<range>& ranges, std::vector<tree_node>& tree, std::size_t begin, std::size_t end) {
        if (begin == end) {
            return no_node;
        }
        const auto middle = begin + (end - begin) / 2, index = tree.size();
        const auto& value = ranges[middle];
        tree.push_back({.value_ = value, .maximum_end_ = value.last_, .maximum_expiry_ = value.expires_});
        const auto left = build_tree(ranges, tree, begin, middle), right = build_tree(ranges, tree, middle + 1, end);
        auto& node = tree[index];
        node.left_ = left;
        node.right_ = right;
        for (const auto child : {left, right}) {
            if (child != no_node) {
                node.maximum_end_ = (std::max)(node.maximum_end_, tree[child].maximum_end_);
                node.maximum_expiry_ = (std::max)(node.maximum_expiry_, tree[child].maximum_expiry_);
            }
        }
        return index;
    }
    static bool in_tree(const std::vector<tree_node>& tree, std::size_t index, const bytes& value, std::uint64_t now) {
        if (index == no_node) {
            return false;
        }
        const auto& node = tree[index];
        if (node.maximum_end_ < value || node.maximum_expiry_ <= now) {
            return false;
        }
        if (in_tree(tree, node.left_, value, now)) {
            return true;
        }
        if (node.value_.first_ > value) {
            return false;
        }
        return (value <= node.value_.last_ && node.value_.expires_ > now) || in_tree(tree, node.right_, value, now);
    }
    static bytes key(const asio::ip::address& address) {
        bytes result{};
        if (address.is_v4()) {
            const auto source = address.to_v4().to_bytes();
            std::ranges::copy(source, result.begin());
        } else {
            result = address.to_v6().to_bytes();
        }
        return result;
    }
    void add_network(std::string_view input, std::uint64_t expires) {
        const auto slash = input.find('/');
        std::error_code error;
        auto address = asio::ip::make_address(input.substr(0, slash), error);
        if (error || (address.is_v6() && address.to_v6().scope_id())) {
            throw std::invalid_argument("invalid IP or CIDR");
        }
        auto prefix = address.is_v4() ? 32u : 128u;
        const auto width = prefix;
        if (slash != std::string_view::npos) {
            const auto suffix = input.substr(slash + 1);
            const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), prefix);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size() || prefix > width) {
                throw std::invalid_argument("invalid CIDR prefix");
            }
        }
        auto normalized_width = width;
        if (address.is_v6() && address.to_v6().is_v4_mapped() && prefix >= 96) {
            address = asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
            prefix -= 96;
            normalized_width = 32;
        }
        range value{.first_ = key(address), .last_ = key(address), .expires_ = expires};
        for (auto bit = prefix; bit < normalized_width; ++bit) {
            const auto mask = static_cast<std::uint8_t>(1u << (7 - bit % 8));
            value.first_[bit / 8] &= static_cast<std::uint8_t>(~mask);
            value.last_[bit / 8] |= mask;
        }
        (address.is_v4() ? ipv4_ : ipv6_).push_back(value);
    }
    std::string type_;
    std::unordered_map<std::string, std::uint64_t> strings_;
    std::vector<range> ipv4_, ipv6_;
    std::vector<tree_node> ipv4_tree_, ipv6_tree_;
    bool has_expiration_{};
};

}  // namespace flexedge::node
