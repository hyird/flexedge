#pragma once

#include <ruvia/core/task.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/web/http_client_handle.h>

#include <array>
#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/certificate/acme_transport.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/outbound_http/client.h"
#include "service/utils/percent_encoding.h"

namespace service::certificate_issuance {

// Trace policies of the ZeroSSL EAB API.
namespace zerossl_trace {

using namespace std::string_view_literals;
using service::attempt_trace::body_rule;
using service::attempt_trace::outbound_policy;

inline constexpr std::array request_id_headers{"x-request-id"sv};
inline constexpr std::array error_code_fields{"type"sv, "error.type"sv};
inline constexpr service::attempt_trace::reply_identity identity{.request_id_headers_ = request_id_headers,
    .error_code_fields_ = error_code_fields,
    .error_code_on_failure_only_ = true};

// eab_kid and eab_hmac_key are deliberately absent.
inline constexpr std::array eab_response{"success"sv, "error.code"sv, "error.type"sv, "error.info"sv};

inline constexpr outbound_policy eab_by_email{.code_ = "zerossl.eab_by_email",
    .request_body_ = body_rule::form,
    .response_body_ = body_rule::json,
    .response_fields_ = eab_response,
    .identity_ = identity};
inline constexpr outbound_policy eab_by_access_key{.code_ = "zerossl.eab_by_access_key",
    .response_body_ = body_rule::json,
    .response_fields_ = eab_response,
    .identity_ = identity};

}  // namespace zerossl_trace

RUVIA_MODEL(zero_ssl_eab_input, RUVIA_OPTIONAL_FIELD_NAME("eab_kid", eab_kid, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("eab_hmac_key", eab_hmac_key, ruvia::string));

template <typename runtime_type>
ruvia::task<service::outbound_http::buffered_response>
send_provider_request(runtime_type& context, service::attempt_trace::recorder& trace,
    const service::outbound_http::quota_scope& scope, const service::attempt_trace::outbound_policy& policy,
    const ruvia::http_client_config& config, ruvia::http_known_method method, std::string_view target,
    std::span<const ruvia::http_header_view> headers, std::optional<std::string_view> body = std::nullopt) {
    co_return co_await service::outbound_http::send_buffered(context, scope,
        {.origin_alias_ = service::config::outbound_origin_alias(config), .policy_ = &policy},
        {.label_ = "证书供应商", .timeout_ = std::chrono::seconds(15)}, trace,
        {
            .method_ = ruvia::known_http_method_token(method),
            .target_ = target,
            .headers_ = headers,
            .content_ = body ? ruvia::http_client_request_content_view::bytes(*body)
                             : ruvia::http_client_request_content_view::none(),
        });
}

template <typename runtime_type>
ruvia::task<void> verify_acme_directory(runtime_type& context, service::attempt_trace::recorder& trace,
    const acme_settings& settings, const service::outbound_http::quota_scope& scope) {
    const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
    const auto response = co_await send_provider_request(context, trace, scope, acme_trace::directory,
        service::config::acme_origin(settings.host_), ruvia::http_known_method::get, settings.directory_target_,
        headers);
    const std::optional<acme_directory_input> parsed =
        ruvia::from_json<acme_directory_input>(response.body(), {.resource_ = context.pool()});
    if (!response.status().is_successful() || !parsed || !parsed->get<"new_nonce">() ||
        !parsed->get<"new_account">() || !parsed->get<"new_order">()) {
        throw service::outbound_http::attempt_error("ACME Directory 检测失败", false);
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
        throw service::outbound_http::attempt_error(failure_message, true);
    }
    const auto& eab_kid = parsed->get<"eab_kid">();
    const auto& eab_hmac_key = parsed->get<"eab_hmac_key">();
    if (!eab_kid || !eab_hmac_key) {
        trace.mark_invalid_response("响应缺少 EAB 凭据");
        throw service::outbound_http::attempt_error(failure_message, true);
    }
    trace.register_secret(eab_kid->view());
    trace.register_secret(eab_hmac_key->view());
    co_return eab_credentials{std::string(eab_kid->view()),
        service::utils::sensitive_string(std::string(eab_hmac_key->view()))};
}

template <typename runtime_type>
ruvia::task<eab_credentials> fetch_zero_ssl_eab_by_email(runtime_type& context,
    service::attempt_trace::recorder& trace, std::string_view account_email,
    const service::outbound_http::quota_scope& scope) {
    trace.register_secret(account_email);
    const auto body = "email=" + service::utils::percent_encode(account_email);
    const std::array headers{
        ruvia::http_header_view{"accept", "application/json"},
        ruvia::http_header_view{"content-type", "application/x-www-form-urlencoded"},
    };
    const auto response = co_await send_provider_request(context, trace, scope, zerossl_trace::eab_by_email,
        service::config::get_outbound_origins().zero_ssl_api_, ruvia::http_known_method::post,
        "/acme/eab-credentials-email", headers, body);
    co_return co_await parse_zero_ssl_eab(context, trace, response, "ZeroSSL 邮箱自动注册或 EAB 凭据获取失败");
}

template <typename runtime_type>
ruvia::task<eab_credentials> fetch_zero_ssl_eab_by_access_key(runtime_type& context,
    service::attempt_trace::recorder& trace, std::string_view access_key,
    const service::outbound_http::quota_scope& scope) {
    trace.register_secret(access_key);
    service::utils::sensitive_string encoded_access_key(service::utils::percent_encode(access_key));
    service::utils::sensitive_string target("/acme/eab-credentials?access_key=" +
                                            std::string(encoded_access_key.view()));
    const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
    const auto response = co_await send_provider_request(context, trace, scope, zerossl_trace::eab_by_access_key,
        service::config::get_outbound_origins().zero_ssl_api_, ruvia::http_known_method::post, target.view(),
        headers);
    co_return co_await parse_zero_ssl_eab(context, trace, response, "ZeroSSL API Access Key 无效或无权获取 EAB 凭据");
}

}  // namespace service::certificate_issuance
