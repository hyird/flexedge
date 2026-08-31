#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include <ruvia/web/ServerConfig.h>

#include "service/features/logging/logger.h"

namespace service::logging {

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

inline void connectionFailure(const ruvia::ConnectionFailureRecord& record) noexcept {
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::exception& error) {
        if (peerDisconnected(error)) {
            return;
        }
        std::string message{"web connection failed"};
        if (!record.remoteAddress().empty()) {
            message.append(" from ").append(record.remoteAddress());
        }
        message.append(": ").append(error.what());
        service::logging::error(std::move(message));
    } catch (...) {
        service::logging::error("web connection failed with non-standard exception");
    }
}

struct ConnectionFailureLogger final {
    void operator()(const ruvia::ConnectionFailureRecord& record) const noexcept {
        connectionFailure(record);
    }
};

} // namespace service::logging
