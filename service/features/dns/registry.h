#pragma once

#include <array>
#include <stdexcept>
#include <string_view>

namespace service::dns {

enum class dns_provider_kind { cloudflare,
    aliyun };

struct dns_provider_descriptor final {
    dns_provider_kind kind_;
    std::string_view code_;
    bool supports_routing_lines_;
    bool supports_proxy_;
};

inline constexpr std::array<dns_provider_descriptor, 2> dns_providers{{
    {.kind_ = dns_provider_kind::cloudflare,
        .code_ = "cloudflare",
        .supports_routing_lines_ = false,
        .supports_proxy_ = true},
    {.kind_ = dns_provider_kind::aliyun,
        .code_ = "aliyun",
        .supports_routing_lines_ = true,
        .supports_proxy_ = false},
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
