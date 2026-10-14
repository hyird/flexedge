#pragma once

#include <ruvia/core/task.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/web/http_client_handle.h>

#include <array>
#include <cctype>
#include <chrono>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/certificate/acme_transport.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/outbound_http/client.h"

namespace service::certificate_issuance {

RUVIA_MODEL(zero_ssl_eab_input, RUVIA_OPTIONAL_FIELD_NAME("eab_kid", eab_kid, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("eab_hmac_key", eab_hmac_key, ruvia::string));

class certificate_provider_client_error final : public std::runtime_error {
public:
    certificate_provider_client_error(std::string_view message, bool retryable)
        : std::runtime_error(std::string(message)),
          retryable_(retryable) {}

    [[nodiscard]] bool retryable() const noexcept {
        return retryable_;
    }

private:
    bool retryable_{false};
};

inline std::string percent_encode(std::string_view value) {
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

template <typename runtime_type>
ruvia::task<service::outbound_http::buffered_response>
send_provider_request(runtime_type& context, service::attempt_trace::recorder& trace,
    service::attempt_trace::outbound_operation operation, const ruvia::http_client_config& config,
    ruvia::http_known_method method, std::string_view target,
    std::span<const ruvia::http_header_view> headers,
    std::optional<std::string_view> body = std::nullopt) {
    try {
        const auto alias = service::config::outbound_origin_alias(config);
        auto&& client = context.get_http_client(alias);
        co_return co_await service::outbound_http::send_buffered(
            client, {.origin_alias_ = alias, .operation_ = operation}, trace,
            {
                .method_ = ruvia::known_http_method_token(method),
                .target_ = target,
                .headers_ = headers,
                .content_ = body ? ruvia::http_client_request_content_view::bytes(*body)
                                 : ruvia::http_client_request_content_view::none(),
            },
            {.timeout_ = std::chrono::seconds(15), .stop_token_ = context.get_stop_token()});
    } catch (const ruvia::http_client_error& error) {
        using code = ruvia::http_client_error::code_type;
        switch (error.code()) {
            case code::timeout:
                throw certificate_provider_client_error("证书供应商请求超时（15 秒）", true);
            case code::resolve_failed:
                throw certificate_provider_client_error("证书供应商域名解析失败", true);
            case code::connect_failed:
                throw certificate_provider_client_error("证书供应商连接失败", true);
            case code::tls_failed:
                throw certificate_provider_client_error("证书供应商 TLS 连接失败", true);
            default:
                throw certificate_provider_client_error("证书供应商 HTTP 客户端失败", true);
        }
    }
}

template <typename runtime_type>
ruvia::task<void> verify_acme_directory(runtime_type& context, service::attempt_trace::recorder& trace,
    const acme_settings& settings) {
    const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
    const auto response = co_await send_provider_request(context, trace,
        service::attempt_trace::outbound_operation::acme_directory, service::config::acme_origin(settings.host_),
        ruvia::http_known_method::get, settings.directory_target_, headers);
    const std::optional<acme_directory_input> parsed =
        ruvia::from_json<acme_directory_input>(response.body(), {.resource_ = context.pool()});
    if (!response.status().is_successful() || !parsed || !parsed->get<"new_nonce">() ||
        !parsed->get<"new_account">() || !parsed->get<"new_order">()) {
        throw certificate_provider_client_error("ACME Directory 检测失败", true);
    }
    co_return;
}

template <typename runtime_type>
ruvia::task<eab_credentials>
parse_zero_ssl_eab(runtime_type& context, service::attempt_trace::recorder& trace,
    const service::outbound_http::buffered_response& response, std::string_view failure_message) {
    const std::optional<zero_ssl_eab_input> parsed =
        ruvia::from_json<zero_ssl_eab_input>(response.body(), {.resource_ = context.pool()});
    if (!response.status().is_successful() || !parsed) {
        throw certificate_provider_client_error(std::string(failure_message), false);
    }
    const auto& eab_kid = parsed->get<"eab_kid">();
    const auto& eab_hmac_key = parsed->get<"eab_hmac_key">();
    if (!eab_kid || !eab_hmac_key) {
        trace.mark_invalid_response("响应缺少 EAB 凭据");
        throw certificate_provider_client_error(std::string(failure_message), false);
    }
    trace.register_secret(eab_kid->view());
    trace.register_secret(eab_hmac_key->view());
    co_return eab_credentials{std::string(eab_kid->view()),
        service::utils::sensitive_string(std::string(eab_hmac_key->view()))};
}

template <typename runtime_type>
ruvia::task<eab_credentials> fetch_zero_ssl_eab_by_email(runtime_type& context,
    service::attempt_trace::recorder& trace, std::string_view account_email) {
    trace.register_secret(account_email);
    const auto body = "email=" + percent_encode(account_email);
    const std::array headers{
        ruvia::http_header_view{"accept", "application/json"},
        ruvia::http_header_view{"content-type", "application/x-www-form-urlencoded"},
    };
    const auto response = co_await send_provider_request(context, trace,
        service::attempt_trace::outbound_operation::zerossl_eab_by_email,
        service::config::get_outbound_origins().zero_ssl_api_, ruvia::http_known_method::post,
        "/acme/eab-credentials-email", headers, body);
    co_return co_await parse_zero_ssl_eab(context, trace, response,
        "ZeroSSL 邮箱自动注册或 EAB 凭据获取失败");
}

template <typename runtime_type>
ruvia::task<eab_credentials> fetch_zero_ssl_eab_by_access_key(runtime_type& context,
    service::attempt_trace::recorder& trace, std::string_view access_key) {
    trace.register_secret(access_key);
    service::utils::sensitive_string encoded_access_key(percent_encode(access_key));
    service::utils::sensitive_string target("/acme/eab-credentials?access_key=" +
                                            std::string(encoded_access_key.view()));
    const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
    const auto response = co_await send_provider_request(context, trace,
        service::attempt_trace::outbound_operation::zerossl_eab_by_access_key,
        service::config::get_outbound_origins().zero_ssl_api_, ruvia::http_known_method::post, target.view(),
        headers);
    co_return co_await parse_zero_ssl_eab(context, trace, response,
        "ZeroSSL API Access Key 无效或无权获取 EAB 凭据");
}

}  // namespace service::certificate_issuance
