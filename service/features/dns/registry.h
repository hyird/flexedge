#pragma once

#include <array>
#include <stdexcept>
#include <string_view>

namespace service::dns {

// What the management API offers per DNS provider. The wire protocol and the
// provider's record rules live in its client (cloudflare.h, aliyun.h).
struct dns_provider_descriptor final {
    std::string_view code_;
    bool supports_routing_lines_;
    bool supports_proxy_;
};

inline constexpr std::array<dns_provider_descriptor, 2> dns_providers{{
    {.code_ = "cloudflare", .supports_routing_lines_ = false, .supports_proxy_ = true},
    {.code_ = "aliyun", .supports_routing_lines_ = true, .supports_proxy_ = false},
}};

[[nodiscard]] inline constexpr const dns_provider_descriptor*
find_dns_provider(std::string_view code) noexcept {
    for (const auto& provider : dns_providers) {
        if (provider.code_ == code) {
            return &provider;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const dns_provider_descriptor& require_dns_provider(std::string_view code) {
    if (const auto* provider = find_dns_provider(code)) {
        return *provider;
    }
    throw std::invalid_argument("不支持的 DNS 服务商类型");
}

}  // namespace service::dns
