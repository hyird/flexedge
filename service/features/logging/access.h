#pragma once

#include <ruvia/web/server_config.h>

#include <iomanip>
#include <sstream>

#include "service/features/logging/logger.h"

namespace service::logging {

inline void access(const ruvia::access_log_record& record) noexcept {
    try {
        const auto micros = record.duration_micros();
        const auto status = record.status();
        std::ostringstream message;
        const auto remote = record.remote_address();
        message << (remote.empty() ? "-" : remote) << ' ' << record.method() << ' ' << record.path()
                << ' ' << status.value() << ' ' << micros / 1000 << '.' << std::setfill('0')
                << std::setw(3) << micros % 1000 << "ms";

        if (status.is_server_error()) {
            error(message.str());
        } else {
            info(message.str());
        }
    } catch (...) {
    }
}

struct access_logger {
    void operator()(const ruvia::access_log_record& record) const noexcept {
        access(record);
    }
};

}  // namespace service::logging
