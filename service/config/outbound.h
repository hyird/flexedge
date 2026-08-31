#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

#include <ruvia/http/HttpClient.h>
#include <ruvia/web/HttpClientHandle.h>

namespace service::config {

inline constexpr std::string_view kCloudflareOriginAlias{"cloudflare"};
inline constexpr std::string_view kAliyunDnsOriginAlias{"aliyun-dns"};
inline constexpr std::string_view kLetsEncryptOriginAlias{"lets-encrypt"};
inline constexpr std::string_view kZeroSslAcmeOriginAlias{"zerossl-acme"};
inline constexpr std::string_view kZeroSslApiOriginAlias{"zerossl-api"};
inline constexpr std::string_view kAliDnsDohOriginAlias{"alidns-doh"};

struct OutboundOrigins final {
    ruvia::HttpClientConfig cloudflare;
    ruvia::HttpClientConfig aliyunDns;
    ruvia::HttpClientConfig letsEncrypt;
    ruvia::HttpClientConfig zeroSslAcme;
    ruvia::HttpClientConfig zeroSslApi;
    ruvia::HttpClientConfig aliDnsDoh;
};

inline OutboundOrigins makeOutboundOrigins(std::string caFile) {
    return {
        .cloudflare =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "api.cloudflare.com",
                .connectionCount = 2,
                .maxBufferedRequests = 128,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(10),
                .maxResponseBytes = 1024 * 1024,
                .caFile = caFile,
                .userAgent = "FlexEdge/0.1",
            },
        .aliyunDns =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "alidns.aliyuncs.com",
                .connectionCount = 2,
                .maxBufferedRequests = 128,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(10),
                .maxResponseBytes = 2 * 1024 * 1024,
                .caFile = caFile,
                .userAgent = "FlexEdge/0.1",
            },
        .letsEncrypt =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "acme-v02.api.letsencrypt.org",
                .connectionCount = 2,
                .maxBufferedRequests = 32,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(30),
                .maxResponseBytes = 2 * 1024 * 1024,
                .caFile = caFile,
                .userAgent = "FlexEdge/0.1",
            },
        .zeroSslAcme =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "acme.zerossl.com",
                .connectionCount = 2,
                .maxBufferedRequests = 32,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(30),
                .maxResponseBytes = 2 * 1024 * 1024,
                .caFile = caFile,
                .userAgent = "FlexEdge/0.1",
            },
        .zeroSslApi =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "api.zerossl.com",
                .connectionCount = 2,
                .maxBufferedRequests = 32,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(15),
                .maxResponseBytes = 1024 * 1024,
                .caFile = caFile,
                .userAgent = "FlexEdge/0.1",
            },
        .aliDnsDoh =
            {
                .scheme = ruvia::HttpScheme::kHttps,
                .host = "dns.alidns.com",
                .connectionCount = 2,
                .maxBufferedRequests = 64,
                .connectTimeout = std::chrono::seconds(5),
                .requestTimeout = std::chrono::seconds(10),
                .maxResponseBytes = 256 * 1024,
                .caFile = std::move(caFile),
                .userAgent = "FlexEdge/0.1",
            },
    };
}

inline const ruvia::HttpClientConfig& outboundOriginConfig(const OutboundOrigins& origins,
                                                           std::string_view alias) {
    if (alias == kCloudflareOriginAlias) {
        return origins.cloudflare;
    }
    if (alias == kAliyunDnsOriginAlias) {
        return origins.aliyunDns;
    }
    if (alias == kLetsEncryptOriginAlias) {
        return origins.letsEncrypt;
    }
    if (alias == kZeroSslAcmeOriginAlias) {
        return origins.zeroSslAcme;
    }
    if (alias == kZeroSslApiOriginAlias) {
        return origins.zeroSslApi;
    }
    if (alias == kAliDnsDohOriginAlias) {
        return origins.aliDnsDoh;
    }
    throw std::invalid_argument("outbound HTTP origin is not configured");
}

inline std::optional<OutboundOrigins>& outboundOriginsStorage() {
    static std::optional<OutboundOrigins> origins;
    return origins;
}

inline void configureOutboundOrigins(OutboundOrigins origins) {
    auto& storage = outboundOriginsStorage();
    if (storage) {
        throw std::logic_error("outbound origins already configured");
    }
    storage.emplace(std::move(origins));
}

inline const OutboundOrigins& outboundOrigins() {
    const auto& storage = outboundOriginsStorage();
    if (!storage) {
        throw std::logic_error("outbound origins are not configured");
    }
    return *storage;
}

inline const ruvia::HttpClientConfig& acmeOrigin(std::string_view host) {
    const auto& origins = outboundOrigins();
    if (host == origins.letsEncrypt.host) {
        return origins.letsEncrypt;
    }
    if (host == origins.zeroSslAcme.host) {
        return origins.zeroSslAcme;
    }
    throw std::invalid_argument("unsupported ACME origin");
}

inline std::string_view outboundOriginAlias(const ruvia::HttpClientConfig& config) {
    const auto& origins = outboundOrigins();
    if (config.host == origins.cloudflare.host) {
        return kCloudflareOriginAlias;
    }
    if (config.host == origins.aliyunDns.host) {
        return kAliyunDnsOriginAlias;
    }
    if (config.host == origins.letsEncrypt.host) {
        return kLetsEncryptOriginAlias;
    }
    if (config.host == origins.zeroSslAcme.host) {
        return kZeroSslAcmeOriginAlias;
    }
    if (config.host == origins.zeroSslApi.host) {
        return kZeroSslApiOriginAlias;
    }
    if (config.host == origins.aliDnsDoh.host) {
        return kAliDnsDohOriginAlias;
    }
    throw std::invalid_argument("outbound HTTP origin is not configured");
}

} // namespace service::config
