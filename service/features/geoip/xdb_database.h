#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>

namespace service::geoip {

struct IpLocation final {
    std::string country;
    std::string display;
};

namespace detail {

namespace fs = std::filesystem;

constexpr std::size_t kXdbHeaderLength = 256;
constexpr std::size_t kXdbVectorLength = 256 * 256 * 8;
constexpr std::size_t kXdbContentOffset = kXdbHeaderLength + kXdbVectorLength;
constexpr std::size_t kXdbV4AddressLength = 4;
constexpr std::size_t kXdbV6AddressLength = 16;
constexpr std::size_t kXdbMaximumRegionLength = 16 * 1024;

[[nodiscard]] inline bool isUnknownLocationPart(std::string_view value) {
    return value.empty() || value == "0" || value == "未知" || value == "未知地区";
}

[[nodiscard]] inline std::optional<IpLocation> parseLocation(std::string_view value) {
    std::vector<std::string_view> parts;
    std::size_t start{};
    while (start <= value.size()) {
        const auto end = value.find('|', start);
        parts.push_back(value.substr(start, end == std::string_view::npos ? std::string_view::npos
                                                                          : end - start));
        if (end == std::string_view::npos) {
            break;
        }
        start = end + 1;
    }
    if (parts.empty()) {
        return std::nullopt;
    }

    constexpr std::array<std::string_view, 14> continents{
        "亚洲", "欧洲",   "非洲",   "北美洲",        "南美洲",        "大洋洲",  "南极洲",
        "Asia", "Europe", "Africa", "North America", "South America", "Oceania", "Antarctica"};
    std::size_t countryIndex{};
    if (std::find(continents.begin(), continents.end(), parts.front()) != continents.end()) {
        countryIndex = 1;
    }
    if (countryIndex >= parts.size() || isUnknownLocationPart(parts[countryIndex])) {
        return std::nullopt;
    }

    IpLocation result{.country = std::string{parts[countryIndex]},
                      .display = std::string{parts[countryIndex]}};
    std::size_t details{};
    for (std::size_t index = countryIndex + 1; index < parts.size() && details < 2; ++index) {
        const auto part = parts[index];
        if (isUnknownLocationPart(part) || part == result.country ||
            result.display.find(part) != std::string::npos) {
            continue;
        }
        result.display += " · ";
        result.display += part;
        ++details;
    }
    return result;
}

[[nodiscard]] inline std::optional<std::string> environmentValue(const char* name) {
#ifdef _WIN32
    char* raw{};
    std::size_t length{};
    if (_dupenv_s(&raw, &length, name) != 0 || !raw) {
        return std::nullopt;
    }
    std::string result{raw, length > 0 ? length - 1 : 0};
    std::free(raw);
    return result;
#else
    const auto* raw = std::getenv(name);
    if (!raw || !*raw) {
        return std::nullopt;
    }
    return std::string{raw};
#endif
}

[[nodiscard]] inline std::uint16_t readLittleEndian16(const std::uint8_t* input) {
    return static_cast<std::uint16_t>(input[0]) |
           (static_cast<std::uint16_t>(input[1]) << 8U);
}

[[nodiscard]] inline std::uint32_t readLittleEndian32(const std::uint8_t* input) {
    return static_cast<std::uint32_t>(input[0]) |
           (static_cast<std::uint32_t>(input[1]) << 8U) |
           (static_cast<std::uint32_t>(input[2]) << 16U) |
           (static_cast<std::uint32_t>(input[3]) << 24U);
}

class XdbReader final {
  public:
    [[nodiscard]] static std::unique_ptr<XdbReader> open(const fs::path& path,
                                                           int expectedIpVersion) {
        std::error_code error;
        const auto size = fs::file_size(path, error);
        if (error || size < kXdbContentOffset ||
            size > static_cast<std::uintmax_t>((std::numeric_limits<std::streamoff>::max)())) {
            return nullptr;
        }

        auto reader = std::unique_ptr<XdbReader>(new XdbReader(path, size));
        reader->input_.open(path, std::ios::binary);
        if (!reader->input_) {
            return nullptr;
        }

        std::array<std::uint8_t, kXdbHeaderLength> header{};
        reader->input_.read(reinterpret_cast<char*>(header.data()),
                            static_cast<std::streamsize>(header.size()));
        if (!reader->input_) {
            return nullptr;
        }
        reader->ipVersion_ = readLittleEndian16(header.data() + 16);
        const auto pointerBytes = readLittleEndian16(header.data() + 18);
        reader->indexStart_ = readLittleEndian32(header.data() + 8);
        reader->indexEnd_ = readLittleEndian32(header.data() + 12);
        reader->addressLength_ = reader->ipVersion_ == 4 ? kXdbV4AddressLength
                                                          : kXdbV6AddressLength;
        reader->recordLength_ = reader->addressLength_ * 2 + 2 + 4;
        if (reader->ipVersion_ != expectedIpVersion || pointerBytes != 4 ||
            reader->indexStart_ < kXdbContentOffset || reader->indexEnd_ < reader->indexStart_ ||
            reader->indexEnd_ + reader->recordLength_ > size ||
            (reader->indexEnd_ - reader->indexStart_) % reader->recordLength_ != 0) {
            return nullptr;
        }

        reader->vectorIndex_.resize(kXdbVectorLength);
        reader->input_.read(reinterpret_cast<char*>(reader->vectorIndex_.data()),
                            static_cast<std::streamsize>(reader->vectorIndex_.size()));
        if (!reader->input_) {
            return nullptr;
        }
        return reader;
    }

    XdbReader(const XdbReader&) = delete;
    XdbReader& operator=(const XdbReader&) = delete;

    [[nodiscard]] std::optional<IpLocation>
    lookup(const std::array<std::uint8_t, kXdbV6AddressLength>& address) const {
        const auto bucket = (static_cast<std::size_t>(address[0]) * 256 + address[1]) * 8;
        if (bucket + 8 > vectorIndex_.size()) {
            return std::nullopt;
        }
        const auto left = readLittleEndian32(vectorIndex_.data() + bucket);
        const auto right = readLittleEndian32(vectorIndex_.data() + bucket + 4);
        if (left == 0 || right == 0 || right < left || left < indexStart_ ||
            right + recordLength_ > fileSize_ || (right - left) % recordLength_ != 0) {
            return std::nullopt;
        }

        std::lock_guard<std::mutex> lock(inputMutex_);
        std::size_t lower{};
        std::size_t upper = (right - left) / recordLength_;
        std::array<std::uint8_t, kXdbV6AddressLength * 2 + 2 + 4> record{};
        while (lower <= upper) {
            const auto middle = lower + (upper - lower) / 2;
            const auto offset = static_cast<std::uint64_t>(left) + middle * recordLength_;
            if (!readLocked(offset, std::span{record}.first(recordLength_))) {
                return std::nullopt;
            }
            const auto leftComparison = compare(address, record.data());
            const auto rightComparison = compare(address, record.data() + addressLength_);
            if (leftComparison < 0) {
                if (middle == 0) {
                    return std::nullopt;
                }
                upper = middle - 1;
                continue;
            }
            if (rightComparison > 0) {
                lower = middle + 1;
                continue;
            }

            const auto regionLength =
                readLittleEndian16(record.data() + addressLength_ * 2);
            const auto regionOffset =
                readLittleEndian32(record.data() + addressLength_ * 2 + 2);
            if (regionLength == 0 || regionLength > kXdbMaximumRegionLength ||
                regionOffset > fileSize_ || regionLength > fileSize_ - regionOffset) {
                return std::nullopt;
            }
            std::vector<std::uint8_t> region(regionLength);
            if (!readLocked(regionOffset, region)) {
                return std::nullopt;
            }
            return parseLocation(
                std::string_view(reinterpret_cast<const char*>(region.data()), region.size()));
        }
        return std::nullopt;
    }

  private:
    XdbReader(fs::path path, std::uintmax_t fileSize) : path_(std::move(path)), fileSize_(fileSize) {}

    [[nodiscard]] int compare(const std::array<std::uint8_t, kXdbV6AddressLength>& address,
                              const std::uint8_t* stored) const {
        for (std::size_t index = 0; index < addressLength_; ++index) {
            const auto databaseByte = ipVersion_ == 4 ? stored[addressLength_ - 1 - index]
                                                      : stored[index];
            if (address[index] < databaseByte) {
                return -1;
            }
            if (address[index] > databaseByte) {
                return 1;
            }
        }
        return 0;
    }

    [[nodiscard]] bool readLocked(std::uint64_t offset, std::span<std::uint8_t> output) const {
        if (offset > fileSize_ || output.size() > fileSize_ - offset ||
            offset > static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
            return false;
        }
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input_.read(reinterpret_cast<char*>(output.data()),
                    static_cast<std::streamsize>(output.size()));
        return input_.good();
    }

    fs::path path_;
    std::uintmax_t fileSize_{};
    int ipVersion_{};
    std::size_t addressLength_{};
    std::size_t recordLength_{};
    std::uint32_t indexStart_{};
    std::uint32_t indexEnd_{};
    std::vector<std::uint8_t> vectorIndex_;
    mutable std::ifstream input_;
    mutable std::mutex inputMutex_;
};

[[nodiscard]] inline std::optional<std::array<std::uint8_t, kXdbV6AddressLength>>
parseAddress(std::string_view value, int expectedIpVersion) {
    std::error_code error;
    const auto address = asio::ip::make_address(std::string{value}, error);
    if (error || (expectedIpVersion == 4 && !address.is_v4()) ||
        (expectedIpVersion == 6 && !address.is_v6())) {
        return std::nullopt;
    }
    std::array<std::uint8_t, kXdbV6AddressLength> bytes{};
    if (address.is_v4()) {
        const auto source = address.to_v4().to_bytes();
        std::copy(source.begin(), source.end(), bytes.begin());
    } else {
        const auto source = address.to_v6().to_bytes();
        std::copy(source.begin(), source.end(), bytes.begin());
    }
    return bytes;
}

} // namespace detail

class XdbDatabase final {
  public:
    XdbDatabase() {
        if (const auto path = detail::environmentValue("FLEXEDGE_XDB_V4_PATH")) {
            ipv4_ = detail::XdbReader::open(*path, 4);
        }
        if (const auto path = detail::environmentValue("FLEXEDGE_XDB_V6_PATH")) {
            ipv6_ = detail::XdbReader::open(*path, 6);
        }
    }

    XdbDatabase(const XdbDatabase&) = delete;
    XdbDatabase& operator=(const XdbDatabase&) = delete;

    [[nodiscard]] bool available() const { return static_cast<bool>(ipv4_) || static_cast<bool>(ipv6_); }

    [[nodiscard]] std::optional<IpLocation> lookup(std::string_view ip) const {
        if (ip.empty() || ip.size() > 64) {
            return std::nullopt;
        }
        if (const auto ipv4 = detail::parseAddress(ip, 4)) {
            return ipv4_ ? ipv4_->lookup(*ipv4) : std::nullopt;
        }
        if (const auto ipv6 = detail::parseAddress(ip, 6)) {
            return ipv6_ ? ipv6_->lookup(*ipv6) : std::nullopt;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::string> country(std::string_view ip) const {
        const auto location = lookup(ip);
        return location ? std::optional<std::string>{location->country} : std::nullopt;
    }

  private:
    std::unique_ptr<detail::XdbReader> ipv4_;
    std::unique_ptr<detail::XdbReader> ipv6_;
};

[[nodiscard]] inline const XdbDatabase& xdbDatabase() {
    static const XdbDatabase database;
    return database;
}

} // namespace service::geoip
