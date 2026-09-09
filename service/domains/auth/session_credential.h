#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include "service/utils/sensitive_string.h"

namespace service::auth {

struct SessionCredential final {
    std::string id;
    service::utils::SensitiveString secret;
};

inline service::utils::SensitiveString encodeSessionCredential(const SessionCredential& credential) {
    auto value = credential.id;
    value.push_back('.');
    value.append(credential.secret.view());
    return service::utils::SensitiveString(std::move(value));
}

inline std::optional<SessionCredential> parseSessionCredential(std::string_view value) {
    const auto separator = value.find('.');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }
    return SessionCredential{
        std::string(value.substr(0, separator)),
        service::utils::SensitiveString(std::string(value.substr(separator + 1)))};
}

} // namespace service::auth
