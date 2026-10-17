#pragma once

#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace flexedge::node {

// Private on-disk metadata. Bounds apply before allocations; network contracts do
// not depend on this format. Incomplete records are never recoverable.
struct cache_disk_record final {
    std::string key_, website_, policy_, url_;
    std::vector<std::pair<std::string, std::string>> headers_, vary_;
    // identity, recency, body size, status, initial age, stored Unix time,
    // expiry Unix time, retention Unix time, browser TTL, stale-if-error seconds,
    // ignore query, website invalidation epoch, policy invalidation epoch.
    std::array<std::uint64_t, 13> values_{};

    bool write(std::ostream& output) const {
        output.write("FECACHE1\n", 9);
        for (const auto* value : {&key_, &website_, &policy_, &url_}) {
            write_string(output, *value);
        }
        for (const auto* fields : {&headers_, &vary_}) {
            write_number(output, fields->size());
            for (const auto& [name, value] : *fields) {
                write_string(output, name);
                write_string(output, value);
            }
        }
        for (const auto value : values_) {
            write_number(output, value);
        }
        return static_cast<bool>(output);
    }
    bool read(std::istream& input) {
        char magic[9]{};
        input.read(magic, 9);
        if (std::string_view(magic, 9) != "FECACHE1\n") {
            return false;
        }
        std::size_t remaining = 1024 * 1024;
        for (auto* value : {&key_, &website_, &policy_, &url_}) {
            if (!read_string(input, *value, remaining)) {
                return false;
            }
        }
        for (auto* fields : {&headers_, &vary_}) {
            std::uint64_t count{};
            if (!read_number(input, count) || count > 256) {
                return false;
            }
            for (std::uint64_t i = 0; i < count; ++i) {
                std::string name, value;
                if (!read_string(input, name, remaining) || !read_string(input, value, remaining)) {
                    return false;
                }
                fields->emplace_back(std::move(name), std::move(value));
            }
        }
        for (auto& value : values_) {
            if (!read_number(input, value)) {
                return false;
            }
        }
        return input.peek() == std::char_traits<char>::eof();
    }

private:
    static void write_number(std::ostream& output, std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            output.put(static_cast<char>((value >> (8 * i)) & 255));
        }
    }
    static bool read_number(std::istream& input, std::uint64_t& value) {
        value = 0;
        for (int i = 0; i < 8; ++i) {
            const auto byte = input.get();
            if (byte == std::char_traits<char>::eof()) {
                return false;
            }
            value |= static_cast<std::uint64_t>(static_cast<unsigned char>(byte)) << (8 * i);
        }
        return true;
    }
    static void write_string(std::ostream& output, const std::string& value) {
        write_number(output, value.size());
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
    static bool read_string(std::istream& input, std::string& value, std::size_t& remaining) {
        std::uint64_t size{};
        if (!read_number(input, size) || size > remaining) {
            return false;
        }
        remaining -= static_cast<std::size_t>(size);
        value.resize(static_cast<std::size_t>(size));
        input.read(value.data(), static_cast<std::streamsize>(size));
        return static_cast<bool>(input);
    }
};

}  // namespace flexedge::node
