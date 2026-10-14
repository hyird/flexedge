#pragma once

#include <ruvia/web/server_config.h>

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

#include "service/common/connection_error.h"
#include "service/features/logging/logger.h"

namespace service::logging {

inline void connection_failure(const ruvia::connection_failure_record& record) noexcept {
    try {
        std::rethrow_exception(record.exception());
    } catch (const std::exception& error) {
        if (service::common::peer_disconnected(error)) {
            return;
        }
        std::string message{"web connection failed"};
        if (!record.remote_address().empty()) {
            message.append(" from ").append(record.remote_address());
        }
        message.append(": ").append(error.what());
        service::logging::error(std::move(message));
    } catch (...) {
        service::logging::error("web connection failed with non-standard exception");
    }
}

struct connection_failure_logger final {
    void operator()(const ruvia::connection_failure_record& record) const noexcept {
        connection_failure(record);
    }
};

}  // namespace service::logging
