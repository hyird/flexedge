#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace service::sync_runtime {

inline constexpr std::size_t kMaxErrorMessageLength{1000};

[[nodiscard]] inline std::string boundedError(std::string_view value) {
    return std::string(value.substr(0, kMaxErrorMessageLength));
}

} // namespace service::sync_runtime
