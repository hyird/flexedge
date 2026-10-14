#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace service::sync_runtime {

inline constexpr std::size_t max_error_message_length{1000};

[[nodiscard]] inline std::string bounded_error(std::string_view value) {
    return std::string(value.substr(0, max_error_message_length));
}

}  // namespace service::sync_runtime
