#pragma once

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include <ruvia/web/ServerConfig.h>

#include "service/features/logging/logger.h"
#include "service/common/connection_error.h"

namespace service::logging {

inline void connectionFailure(const ruvia::ConnectionFailureRecord& record) noexcept {
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::exception& error) {
        if (service::common::peerDisconnected(error)) {
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
