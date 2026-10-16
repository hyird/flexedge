#pragma once

#include <string_view>

#include "service/features/outbound_http/error.h"

namespace service::dns {

enum class provider_error_code {
    credential_invalid,
    authorization_failed,
    upstream_failed,
    zone_not_found,
    dns_failed,
    // More than one provider record matches the desired one; none can be taken over safely.
    record_conflict,
    // The name already holds records the new one cannot share.
    record_name_conflict,
    record_not_found,
};

// A failure a DNS provider reported. Credentials, authorization, a missing
// zone and an ambiguous record do not change by retrying: they are permanent.
class provider_error final : public service::outbound_http::attempt_error {
public:
    provider_error(provider_error_code code, std::string_view message)
        : attempt_error(message, permanent_code(code)),
          code_(code) {}

    [[nodiscard]] provider_error_code code() const noexcept {
        return code_;
    }

    [[nodiscard]] static constexpr bool permanent_code(provider_error_code code) noexcept {
        return code == provider_error_code::credential_invalid || code == provider_error_code::authorization_failed ||
               code == provider_error_code::zone_not_found || code == provider_error_code::record_conflict;
    }

private:
    provider_error_code code_;
};

}  // namespace service::dns
