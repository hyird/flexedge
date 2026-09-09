#pragma once
#include <exception>
#include <string_view>
#include <system_error>

namespace service::common {
inline bool peerDisconnected(const std::exception& error) noexcept {
    if (const auto* systemError = dynamic_cast<const std::system_error*>(&error)) {
        const auto code = systemError->code();
        if (code == std::errc::broken_pipe || code == std::errc::connection_reset ||
            code == std::errc::connection_aborted) {
            return true;
        }
    }
    const std::string_view message{error.what()};
    return message.contains("Broken pipe") || message.contains("Connection reset by peer");
}

inline bool sseClientDisconnected(const std::exception& error) noexcept {
    return peerDisconnected(error) ||
           std::string_view(error.what()).contains("redis operation cancelled");
}
} // namespace service::common
