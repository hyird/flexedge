#pragma once

#include "service/features/dns_sync/model.h"

namespace service::dns_sync {

[[nodiscard]] inline bool isEnabledLine(const ZoneLineRuntimeData& line) noexcept {
    return line.code && line.status && *line.status == "enabled";
}

} // namespace service::dns_sync
