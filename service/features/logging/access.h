#pragma once

#include <iomanip>
#include <sstream>

#include <ruvia/web/ServerConfig.h>

#include "service/features/logging/logger.h"

namespace service::logging {

inline void access(const ruvia::AccessLogRecord& record) noexcept {
    try {
        const auto micros = record.durationMicros();
        const auto status = record.status();
        std::ostringstream message;
        const auto remote = record.remoteAddress();
        message << (remote.empty() ? "-" : remote) << ' ' << record.method() << ' ' << record.path()
                << ' ' << status.value() << ' ' << micros / 1000 << '.' << std::setfill('0')
                << std::setw(3) << micros % 1000 << "ms";

        if (status.isServerError()) {
            error(message.str());
        } else {
            info(message.str());
        }
    } catch (...) {
    }
}

struct AccessLogger {
    void operator()(const ruvia::AccessLogRecord& record) const noexcept { access(record); }
};

} // namespace service::logging
