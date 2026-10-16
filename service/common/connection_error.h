#pragma once
#include <exception>
#include <string_view>
#include <system_error>

#include <asio/error.hpp>

namespace service::common {
// The peer went away: the connection was closed, reset or aborted, or the
// operation on it was cancelled because the connection ended.
inline bool peer_disconnected(const std::error_code& code) noexcept {
    return code == std::errc::broken_pipe || code == std::errc::connection_reset ||
           code == std::errc::connection_aborted || code == asio::error::operation_aborted ||
           code == asio::error::eof;
}

inline bool peer_disconnected(const std::exception& error) noexcept {
    if (const auto* system_error = dynamic_cast<const std::system_error*>(&error);
        system_error != nullptr && peer_disconnected(system_error->code())) {
        return true;
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
