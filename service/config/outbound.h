#pragma once

#include <ruvia/http/http_client.h>
#include <ruvia/web/http_client_types.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>

namespace service::config {

inline constexpr std::string_view cloudflare_origin_alias{"cloudflare"};
inline constexpr std::string_view aliyun_dns_origin_alias{"aliyun-dns"};
inline constexpr std::string_view lets_encrypt_origin_alias{"lets-encrypt"};
inline constexpr std::string_view zero_ssl_acme_origin_alias{"zerossl-acme"};
inline constexpr std::string_view zero_ssl_api_origin_alias{"zerossl-api"};
inline constexpr std::string_view ali_dns_doh_origin_alias{"alidns-doh"};

struct outbound_origins final {
    ruvia::http_client_config cloudflare_;
    ruvia::http_client_config aliyun_dns_;
    ruvia::http_client_config lets_encrypt_;
    ruvia::http_client_config zero_ssl_acme_;
    ruvia::http_client_config zero_ssl_api_;
    ruvia::http_client_config ali_dns_doh_;
};

inline outbound_origins make_outbound_origins(std::string ca_file) {
    return {
        .cloudflare_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "api.cloudflare.com",
                .connection_count_ = 2,
                .max_buffered_requests_ = 128,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(10),
                .max_response_bytes_ = 1024 * 1024,
                .tls_ = {.ca_file_ = ca_file},
                .user_agent_ = "FlexEdge/0.1",
            },
        .aliyun_dns_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "alidns.aliyuncs.com",
                .connection_count_ = 2,
                .max_buffered_requests_ = 128,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(10),
                .max_response_bytes_ = 2 * 1024 * 1024,
                .tls_ = {.ca_file_ = ca_file},
                .user_agent_ = "FlexEdge/0.1",
            },
        .lets_encrypt_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "acme-v02.api.letsencrypt.org",
                .connection_count_ = 2,
                .max_buffered_requests_ = 32,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(30),
                .max_response_bytes_ = 2 * 1024 * 1024,
                .tls_ = {.ca_file_ = ca_file},
                .user_agent_ = "FlexEdge/0.1",
            },
        .zero_ssl_acme_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "acme.zerossl.com",
                .connection_count_ = 2,
                .max_buffered_requests_ = 32,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(30),
                .max_response_bytes_ = 2 * 1024 * 1024,
                .tls_ = {.ca_file_ = ca_file},
                .user_agent_ = "FlexEdge/0.1",
            },
        .zero_ssl_api_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "api.zerossl.com",
                .connection_count_ = 2,
                .max_buffered_requests_ = 32,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(15),
                .max_response_bytes_ = 1024 * 1024,
                .tls_ = {.ca_file_ = ca_file},
                .user_agent_ = "FlexEdge/0.1",
            },
        .ali_dns_doh_ =
            {
                .scheme_ = ruvia::http_scheme::https,
                .host_ = "dns.alidns.com",
                .connection_count_ = 2,
                .max_buffered_requests_ = 64,
                .connect_timeout_ = std::chrono::seconds(5),
                .request_timeout_ = std::chrono::seconds(10),
                .max_response_bytes_ = 256 * 1024,
                .tls_ = {.ca_file_ = std::move(ca_file)},
                .user_agent_ = "FlexEdge/0.1",
            },
    };
}

inline const ruvia::http_client_config& outbound_origin_config(const outbound_origins& origins,
    std::string_view alias) {
    if (alias == cloudflare_origin_alias) {
        return origins.cloudflare_;
    }
    if (alias == aliyun_dns_origin_alias) {
        return origins.aliyun_dns_;
    }
    if (alias == lets_encrypt_origin_alias) {
        return origins.lets_encrypt_;
    }
    if (alias == zero_ssl_acme_origin_alias) {
        return origins.zero_ssl_acme_;
    }
    if (alias == zero_ssl_api_origin_alias) {
        return origins.zero_ssl_api_;
    }
    if (alias == ali_dns_doh_origin_alias) {
        return origins.ali_dns_doh_;
    }
    throw std::invalid_argument("outbound HTTP origin is not configured");
}

inline std::optional<outbound_origins>& outbound_origins_storage() {
    static std::optional<outbound_origins> origins;
    return origins;
}

inline void configure_outbound_origins(outbound_origins origins) {
    auto& storage = outbound_origins_storage();
    if (storage) {
        throw std::logic_error("outbound origins already configured");
    }
    storage.emplace(std::move(origins));
}

inline const outbound_origins& get_outbound_origins() {
    const auto& storage = outbound_origins_storage();
    if (!storage) {
        throw std::logic_error("outbound origins are not configured");
    }
    return *storage;
}

inline const ruvia::http_client_config& acme_origin(std::string_view host) {
    const auto& origins = get_outbound_origins();
    if (host == origins.lets_encrypt_.host_) {
        return origins.lets_encrypt_;
    }
    if (host == origins.zero_ssl_acme_.host_) {
        return origins.zero_ssl_acme_;
    }
    throw std::invalid_argument("unsupported ACME origin");
}

inline std::string_view outbound_origin_alias(const ruvia::http_client_config& config) {
    const auto& origins = get_outbound_origins();
    if (config.host_ == origins.cloudflare_.host_) {
        return cloudflare_origin_alias;
    }
    if (config.host_ == origins.aliyun_dns_.host_) {
        return aliyun_dns_origin_alias;
    }
    if (config.host_ == origins.lets_encrypt_.host_) {
        return lets_encrypt_origin_alias;
    }
    if (config.host_ == origins.zero_ssl_acme_.host_) {
        return zero_ssl_acme_origin_alias;
    }
    if (config.host_ == origins.zero_ssl_api_.host_) {
        return zero_ssl_api_origin_alias;
    }
    if (config.host_ == origins.ali_dns_doh_.host_) {
        return ali_dns_doh_origin_alias;
    }
    throw std::invalid_argument("outbound HTTP origin is not configured");
}

}  // namespace service::config
