#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/utils/sensitive_string.h"

namespace service::auth {

struct session_credential final {
    std::string id_;
    service::utils::sensitive_string secret_;
};

inline service::utils::sensitive_string encode_session_credential(const session_credential& credential) {
    auto value = credential.id_;
    value.push_back('.');
    value.append(credential.secret_.view());
    return service::utils::sensitive_string(std::move(value));
}

inline std::optional<session_credential> parse_session_credential(std::string_view value) {
    const auto separator = value.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }
    return session_credential{
        std::string(value.substr(0, separator)),
        service::utils::sensitive_string(std::string(value.substr(separator + 1)))};
}

}  // namespace service::auth
