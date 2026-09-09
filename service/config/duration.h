#pragma once
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::config {

inline std::chrono::seconds parsePositiveDuration(std::string_view value, std::string_view configName) {
    if (value.empty()) {
        throw std::runtime_error(std::string(configName) + " must not be empty");
    }

    std::string_view number = value;
    std::int64_t multiplier = 1;
    const char suffix = value.back();
    if (suffix < '0' || suffix > '9') {
        number.remove_suffix(1);
        switch (suffix) {
        case 's':
            multiplier = 1;
            break;
        case 'm':
            multiplier = 60;
            break;
        case 'h':
            multiplier = 60 * 60;
            break;
        case 'd':
            multiplier = 60 * 60 * 24;
            break;
        default:
            throw std::runtime_error(std::string(configName) + " must use an s, m, h, or d suffix");
        }
    }

    std::int64_t count = 0;
    const auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), count);
    if (error != std::errc{} || end != number.data() + number.size() || count <= 0 ||
        count > std::numeric_limits<std::int64_t>::max() / multiplier) {
        throw std::runtime_error(std::string(configName) + " must be a positive duration");
    }
    return std::chrono::seconds(count * multiplier);
}

} // namespace service::config
