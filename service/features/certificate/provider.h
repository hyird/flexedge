#pragma once

#include <array>
#include <chrono>
#include <cctype>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/core/Task.h>
#include <ruvia/http/HttpKnownMethod.h>
#include <ruvia/web/HttpClientHandle.h>
#include <ruvia/web/Model.h>

#include "service/config/outbound.h"
#include "service/features/certificate/acme.h"
#include "service/features/outbound_http/client.h"

namespace service::certificate_issuance {

RUVIA_REQUEST_MODEL(ZeroSslEabInput, RUVIA_OPTIONAL_FIELD_NAME("eab_kid", eabKid, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("eab_hmac_key", eabHmacKey, ruvia::String));

class CertificateProviderClientError final : public std::runtime_error {
  public:
    CertificateProviderClientError(std::string_view message, bool retryable)
        : std::runtime_error(std::string(message)), retryable_(retryable) {}

    [[nodiscard]] bool retryable() const noexcept { return retryable_; }

  private:
    bool retryable_{false};
};

inline std::string percentEncode(std::string_view value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size() * 3);
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(digits[ch >> 4]);
            output.push_back(digits[ch & 0x0F]);
        }
    }
    return output;
}

template <typename Runtime>
ruvia::Task<service::outbound_http::BufferedResponse>
sendProviderRequest(Runtime& context, const ruvia::HttpClientConfig& config,
                    ruvia::HttpKnownMethod method, std::string_view target,
                    std::span<const ruvia::HttpHeaderView> headers,
                    std::optional<std::string_view> body = std::nullopt) {
    try {
        auto&& client = context.httpClient(service::config::outboundOriginAlias(config));
        co_return co_await service::outbound_http::sendBuffered(
            client,
            {
                .method = ruvia::knownHttpMethodToken(method),
                .target = target,
                .headers = headers,
                .content = body ? ruvia::HttpClientRequestContentView::bytes(*body)
                                : ruvia::HttpClientRequestContentView::none(),
            },
            {.timeout = std::chrono::seconds(15), .stopToken = context.stopToken()});
    } catch (const ruvia::HttpClientError& error) {
        using Code = ruvia::HttpClientError::Code;
        switch (error.code()) {
        case Code::kTimeout:
            throw CertificateProviderClientError("证书供应商请求超时（15 秒）", true);
        case Code::kResolveFailed:
            throw CertificateProviderClientError("证书供应商域名解析失败", true);
        case Code::kConnectFailed:
            throw CertificateProviderClientError("证书供应商连接失败", true);
        case Code::kTlsFailed:
            throw CertificateProviderClientError("证书供应商 TLS 连接失败", true);
        default:
            throw CertificateProviderClientError("证书供应商 HTTP 客户端失败", true);
        }
    }
}

template <typename Runtime>
ruvia::Task<void> verifyAcmeDirectory(Runtime& context, const AcmeSettings& settings) {
    const std::array headers{ruvia::HttpHeaderView{"accept", "application/json"}};
    const auto response = co_await sendProviderRequest(
        context, service::config::acmeOrigin(settings.host), ruvia::HttpKnownMethod::kGet,
        settings.directoryTarget, headers);
    const std::optional<AcmeDirectoryInput> parsed =
        ruvia::fromJson<AcmeDirectoryInput>(response.body(), {.resource = context.resource()});
    if (!response.status().isSuccessful() || !parsed || !parsed->get<"newNonce">() ||
        !parsed->get<"newAccount">() || !parsed->get<"newOrder">()) {
        throw CertificateProviderClientError("ACME Directory 检测失败", true);
    }
    co_return;
}

template <typename Runtime>
ruvia::Task<EabCredentials>
parseZeroSslEab(Runtime& context, const service::outbound_http::BufferedResponse& response,
                std::string_view failureMessage) {
    const std::optional<ZeroSslEabInput> parsed =
        ruvia::fromJson<ZeroSslEabInput>(response.body(), {.resource = context.resource()});
    if (!response.status().isSuccessful() || !parsed) {
        throw CertificateProviderClientError(std::string(failureMessage), false);
    }
    const auto& eabKid = parsed->get<"eabKid">();
    const auto& eabHmacKey = parsed->get<"eabHmacKey">();
    if (!eabKid || !eabHmacKey) {
        throw CertificateProviderClientError(std::string(failureMessage), false);
    }
    co_return EabCredentials{std::string(eabKid->view()),
                             service::utils::SensitiveString(std::string(eabHmacKey->view()))};
}

template <typename Runtime>
ruvia::Task<EabCredentials> fetchZeroSslEabByEmail(Runtime& context,
                                                   std::string_view accountEmail) {
    const auto body = "email=" + percentEncode(accountEmail);
    const std::array headers{
        ruvia::HttpHeaderView{"accept", "application/json"},
        ruvia::HttpHeaderView{"content-type", "application/x-www-form-urlencoded"},
    };
    const auto response = co_await sendProviderRequest(
        context, service::config::outboundOrigins().zeroSslApi, ruvia::HttpKnownMethod::kPost,
        "/acme/eab-credentials-email", headers, body);
    co_return co_await parseZeroSslEab(context, response,
                                       "ZeroSSL 邮箱自动注册或 EAB 凭据获取失败");
}

template <typename Runtime>
ruvia::Task<EabCredentials> fetchZeroSslEabByAccessKey(Runtime& context,
                                                       std::string_view accessKey) {
    service::utils::SensitiveString encodedAccessKey(percentEncode(accessKey));
    service::utils::SensitiveString target("/acme/eab-credentials?access_key=" +
                                           std::string(encodedAccessKey.view()));
    const std::array headers{ruvia::HttpHeaderView{"accept", "application/json"}};
    const auto response =
        co_await sendProviderRequest(context, service::config::outboundOrigins().zeroSslApi,
                                     ruvia::HttpKnownMethod::kPost, target.view(), headers);
    co_return co_await parseZeroSslEab(context, response,
                                       "ZeroSSL API Access Key 无效或无权获取 EAB 凭据");
}

} // namespace service::certificate_issuance
