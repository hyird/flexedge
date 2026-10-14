#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "service/common/http.h"
#include "service/common/uuid.h"

namespace service::website {

// `certificate_id` list filter: websites bound to that certificate.
inline std::optional<std::string> parse_certificate_id_filter(std::optional<std::string_view> value) {
    if (!value) {
        return std::nullopt;
    }
    auto id = service::common::parse_uuid(value);
    if (!id) {
        service::common::throw_app_error(service::common::validation_error_code,
            "certificate_id 必须是 UUID", 400);
    }
    return id;
}

}  // namespace service::website
