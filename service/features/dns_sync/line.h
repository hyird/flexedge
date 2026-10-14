#pragma once

#include "service/features/dns_sync/model.h"

namespace service::dns_sync {

[[nodiscard]] inline bool is_enabled_line(const zone_line_runtime_data& line) noexcept {
    return line.code_ && line.status_ && *line.status_ == "enabled";
}

}  // namespace service::dns_sync
