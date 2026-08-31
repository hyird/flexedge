#pragma once

#include <array>
#include <stdexcept>
#include <string_view>

namespace service::dns {

enum class DnsProviderKind { cloudflare, aliyun };

struct DnsProviderDescriptor final {
    DnsProviderKind kind;
    std::string_view code;
    bool supportsRoutingLines;
    bool supportsProxy;
};

inline constexpr std::array<DnsProviderDescriptor, 2> kDnsProviders{{
    {.kind = DnsProviderKind::cloudflare,
     .code = "cloudflare",
     .supportsRoutingLines = false,
     .supportsProxy = true},
    {.kind = DnsProviderKind::aliyun,
     .code = "aliyun",
     .supportsRoutingLines = true,
     .supportsProxy = false},
}};

[[nodiscard]] inline constexpr const DnsProviderDescriptor*
findDnsProvider(std::string_view code) noexcept {
    for (const auto& provider : kDnsProviders) {
        if (provider.code == code) {
            return &provider;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const DnsProviderDescriptor& requireDnsProvider(std::string_view code) {
    if (const auto* provider = findDnsProvider(code)) {
        return *provider;
    }
    throw std::invalid_argument("不支持的 DNS 服务商类型");
}

} // namespace service::dns
