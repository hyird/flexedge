#pragma once
#include <exception>
#include <string_view>
#include <system_error>

namespace service::common {
inline bool peer_disconnected(const std::exception& error) noexcept {
    if (const auto* system_error = dynamic_cast<const std::system_error*>(&error)) {
        const auto code = system_error->code();
        if (code == std::errc::broken_pipe || code == std::errc::connection_reset ||
            code == std::errc::connection_aborted) {
            return true;
        }
    }
    const std::string_view message{error.what()};
    return message.find("Broken pipe") != std::string_view::npos ||
           message.find("Connection reset by peer") != std::string_view::npos;
}

inline bool sse_client_disconnected(const std::exception& error) noexcept {
    return peer_disconnected(error) ||
           std::string_view(error.what()).find("redis operation cancelled") !=
               std::string_view::npos;
}
}  // namespace service::common
