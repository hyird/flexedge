#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

#include "service/features/geoip/location.h"

namespace service::geoip {

namespace detail {

namespace fs = std::filesystem;

constexpr std::size_t xdb_header_length = 256;
constexpr std::size_t xdb_vector_length = 256 * 256 * 8;
constexpr std::size_t xdb_content_offset = xdb_header_length + xdb_vector_length;
constexpr std::size_t xdb_v4_address_length = 4;
constexpr std::size_t xdb_v6_address_length = 16;
constexpr std::size_t xdb_maximum_region_length = 16 * 1024;

[[nodiscard]] inline std::uint16_t read_little_endian16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           (static_cast<std::uint16_t>(input[1]) << 8U);
}

[[nodiscard]] inline std::uint32_t read_little_endian32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

// One XDB file read completely when opened; lookups only read the immutable bytes.
class xdb_reader final {
public:
    [[nodiscard]] static std::unique_ptr<const xdb_reader> open(const fs::path& path,
        int expected_ip_version) {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error || size < xdb_content_offset ||
            size > static_cast<std::uintmax_t>((std::numeric_limits<std::streamsize>::max)())) {
            return nullptr;
        }
        std::vector<std::uint8_t> content(static_cast<std::size_t>(size));
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char*>(content.data()),
            static_cast<std::streamsize>(content.size()));
        if (!input) {
            return nullptr;
        }

        auto reader = std::unique_ptr<xdb_reader>(new xdb_reader(std::move(content)));
        const auto* header = reader->content_.data();
        reader->ip_version_ = read_little_endian16(header + 16);
        const auto pointer_bytes = read_little_endian16(header + 18);
        reader->index_start_ = read_little_endian32(header + 8);
        reader->index_end_ = read_little_endian32(header + 12);
        reader->address_length_ = reader->ip_version_ == 4 ? xdb_v4_address_length
                                                           : xdb_v6_address_length;
        reader->record_length_ = reader->address_length_ * 2 + 2 + 4;
        if (reader->ip_version_ != expected_ip_version || pointer_bytes != 4 ||
            reader->index_start_ < xdb_content_offset || reader->index_end_ < reader->index_start_ ||
            reader->index_end_ + reader->record_length_ > reader->content_.size() ||
            (reader->index_end_ - reader->index_start_) % reader->record_length_ != 0) {
            return nullptr;
        }
        return reader;
    }

    xdb_reader(const xdb_reader&) = delete;
    xdb_reader& operator=(const xdb_reader&) = delete;

    [[nodiscard]] std::optional<ip_location>
    lookup(const std::array<std::uint8_t, xdb_v6_address_length>& address) const {
        const auto size = content_.size();
        const auto bucket =
            xdb_header_length + (static_cast<std::size_t>(address[0]) * 256 + address[1]) * 8;
        const auto left = read_little_endian32(content_.data() + bucket);
        const auto right = read_little_endian32(content_.data() + bucket + 4);
        if (left == 0 || right == 0 || right < left || left < index_start_ ||
            right > index_end_ || (left - index_start_) % record_length_ != 0 ||
            right + record_length_ > size || (right - left) % record_length_ != 0) {
            return std::nullopt;
        }

        std::size_t lower{};
        std::size_t upper = (right - left) / record_length_;
        while (lower <= upper) {
            const auto middle = lower + (upper - lower) / 2;
            const auto* record = content_.data() + left + middle * record_length_;
            const auto left_comparison = compare(address, record);
            const auto right_comparison = compare(address, record + address_length_);
            if (left_comparison < 0) {
                if (middle == 0) {
                    return std::nullopt;
                }
                upper = middle - 1;
                continue;
            }
            if (right_comparison > 0) {
                lower = middle + 1;
                continue;
            }

            const auto region_length = read_little_endian16(record + address_length_ * 2);
            const auto region_offset = read_little_endian32(record + address_length_ * 2 + 2);
            if (region_length == 0 || region_length > xdb_maximum_region_length ||
                region_offset > size || region_length > size - region_offset) {
                return std::nullopt;
            }
            return parse_location(std::string_view(
                reinterpret_cast<const char*>(content_.data() + region_offset), region_length));
        }
        return std::nullopt;
    }

private:
    explicit xdb_reader(std::vector<std::uint8_t> content)
        : content_(std::move(content)) {}

    [[nodiscard]] int compare(const std::array<std::uint8_t, xdb_v6_address_length>& address,
        const std::uint8_t* stored) const {
        for (std::size_t index = 0; index < address_length_; ++index) {
            const auto database_byte = ip_version_ == 4 ? stored[address_length_ - 1 - index]
                                                        : stored[index];
            if (address[index] < database_byte) {
                return -1;
            }
            if (address[index] > database_byte) {
                return 1;
            }
        }
        return 0;
    }

    std::vector<std::uint8_t> content_;
    int ip_version_{};
    std::size_t address_length_{};
    std::size_t record_length_{};
    std::uint32_t index_start_{};
    std::uint32_t index_end_{};
};

[[nodiscard]] inline std::optional<std::array<std::uint8_t, xdb_v6_address_length>>
parse_address(std::string_view value, int expected_ip_version) {
    std::error_code error;
    const auto address = asio::ip::make_address(std::string{value}, error);
    if (error || (expected_ip_version == 4 && !address.is_v4()) ||
        (expected_ip_version == 6 && !address.is_v6())) {
        return std::nullopt;
    }
    std::array<std::uint8_t, xdb_v6_address_length> bytes{};
    if (address.is_v4()) {
        const auto source = address.to_v4().to_bytes();
        std::copy(source.begin(), source.end(), bytes.begin());
    } else {
        const auto source = address.to_v6().to_bytes();
        std::copy(source.begin(), source.end(), bytes.begin());
    }
    return bytes;
}

}  // namespace detail

// Immutable after construction: every lookup is a pure in-memory search, safe to
// share between Worker threads without locking.
class xdb_database final {
public:
    xdb_database(const std::filesystem::path& ipv4_path, const std::filesystem::path& ipv6_path) {
        if (!ipv4_path.empty()) {
            ipv4_ = detail::xdb_reader::open(ipv4_path, 4);
        }
        if (!ipv6_path.empty()) {
            ipv6_ = detail::xdb_reader::open(ipv6_path, 6);
        }
    }

    xdb_database(const xdb_database&) = delete;
    xdb_database& operator=(const xdb_database&) = delete;

    [[nodiscard]] bool available() const {
        return static_cast<bool>(ipv4_) || static_cast<bool>(ipv6_);
    }

    [[nodiscard]] std::optional<ip_location> lookup(std::string_view ip) const {
        if (ip.empty() || ip.size() > 64) {
            return std::nullopt;
        }
        if (const auto ipv4 = detail::parse_address(ip, 4)) {
            return ipv4_ ? ipv4_->lookup(*ipv4) : std::nullopt;
        }
        if (const auto ipv6 = detail::parse_address(ip, 6)) {
            return ipv6_ ? ipv6_->lookup(*ipv6) : std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> country(std::string_view ip) const {
        const auto location = lookup(ip);
        if (!location || location->country_.empty()) {
            return std::nullopt;
        }
        return location->country_;
    }

private:
    std::unique_ptr<const detail::xdb_reader> ipv4_;
    std::unique_ptr<const detail::xdb_reader> ipv6_;
};

}  // namespace service::geoip
