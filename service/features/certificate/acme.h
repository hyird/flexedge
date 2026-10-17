#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/http_client_handle.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/certificate/acme_jws.h"
#include "service/features/certificate/acme_transport.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/outbound_http/client.h"

namespace service::certificate_issuance {

namespace detail {

inline std::string json_string(auto&& model, std::pmr::memory_resource* resource) {
    const auto value = ruvia::to_json(std::forward<decltype(model)>(model), {.resource_ = resource});
    return std::string(value.data(), value.size());
}

struct directory final {
    std::string new_nonce_;
    std::string new_account_;
    std::string new_order_;
};

struct challenge final {
    std::string url_;
    std::string token_;
};

inline std::string optional_string(const std::optional<ruvia::string>& value) {
    return value ? std::string(value->view()) : std::string{};
}

inline std::string problem_detail(const std::optional<acme_problem_input>& problem) {
    if (!problem) {
        return {};
    }
    if (const auto& detail_value = problem->get<"detail">(); detail_value) {
        return std::string(detail_value->view());
    }
    if (const auto& type_value = problem->get<"type">(); type_value) {
        return std::string(type_value->view());
    }
    return {};
}

[[nodiscard]] inline bool permanent_problem(std::string_view type, ruvia::http_status_code status) {
    if (status.value() == 429 || status.is_server_error()) {
        return false;
    }
    return type.ends_with(":malformed") || type.ends_with(":unauthorized") ||
           type.ends_with(":rejectedIdentifier") || type.ends_with(":accountDoesNotExist");
}

// The error of a failed ACME reply: the fallback, then the problem's
// `detail` when the reply carries one. The text passes the attempt's
// redaction (EAB key and kid, account e-mail, any echoed credential) before
// it can reach the task error or the server log.
[[nodiscard]] inline acme_error acme_problem_error(service::attempt_trace::recorder& trace, std::string_view body,
    ruvia::http_status_code status, std::string_view fallback, std::pmr::memory_resource* resource) {
    const std::optional<acme_problem_input> parsed = ruvia::from_json<acme_problem_input>(body, {.resource_ = resource});
    std::string type;
    std::string message(fallback);
    if (parsed) {
        type = optional_string(parsed->get<"type">());
        const auto detail_value = optional_string(parsed->get<"detail">());
        if (!detail_value.empty()) {
            message.append("：").append(detail_value);
        }
    }
    return acme_error(trace.redact_error(message), permanent_problem(type, status));
}

}  // namespace detail

// An ACME order (RFC 8555 §7.1.3) and the URL it is polled at.
struct acme_order final {
    std::string url_;
    std::string status_;
    std::vector<std::string> authorizations_;
    std::string finalize_;
    std::string certificate_;
    std::string error_;
};

// An ACME authorization (RFC 8555 §7.1.4) with its DNS-01 challenge.
struct acme_authorization final {
    std::string url_;
    std::string status_;
    std::string identifier_;
    std::optional<detail::challenge> dns_challenge_;
    std::string error_;
};

// The TXT record that answers one DNS-01 challenge.
struct dns_challenge_record final {
    std::string name_;
    std::string content_;
};

// Stepwise ACME protocol client (RFC 8555) for one attempt: every call is a
// protocol exchange; ordering, persistence and leases belong to the caller.
// Polls only the ACME server, bounded by the caller's deadline.
class acme_client final {
public:
    acme_client(acme_settings settings, service::attempt_trace::recorder& trace,
        service::outbound_http::quota_scope scope)
        : settings_(std::move(settings)),
          trace_(trace),
          scope_(std::move(scope)) {}

    [[nodiscard]] const acme_settings& settings() const noexcept {
        return settings_;
    }

    [[nodiscard]] service::utils::sensitive_string generate_account_key() const {
        return service::utils::sensitive_string(
            detail::private_key_pem(detail::generate_rsa_key().get()));
    }

    template <typename runtime_type>
    ruvia::task<std::string> register_account(runtime_type& context, std::string_view private_key_pem,
        std::optional<std::string_view> account_email,
        const std::optional<eab_credentials>& eab) {
        if (account_email) {
            trace_.register_secret(*account_email);
        }
        if (eab) {
            trace_.register_secret(eab->key_id_);
            trace_.register_secret(eab->hmac_key_.view());
        }
        auto key = detail::load_private_key(private_key_pem);
        const auto* directory = co_await load_directory(context);
        acme_account_payload_output payload({.resource_ = context.pool()});
        payload.set<"terms_agreed">(true);
        if (account_email && !account_email->empty()) {
            const auto contact = "mailto:" + std::string(*account_email);
            payload.ensure<"contact">().emplace_back(
                contact, ruvia::model_options{.resource_ = context.pool()});
        }
        if (eab) {
            payload.set<"external_account_binding">(
                build_external_account_binding(context, key.get(), directory->new_account_, *eab));
        }
        const auto response =
            co_await signed_post(context, directory->new_account_, key.get(), std::nullopt,
                detail::json_string(std::move(payload), context.pool()),
                acme_trace::new_account);
        const auto location = response.header("location");
        if (!location || location->empty()) {
            throw acme_error("ACME 账户响应缺少 Location", true);
        }
        (void)target_from_url(*location);
        trace_.log(service::attempt_trace::log_level::info, "已注册新的 ACME 账户");
        co_return std::string(*location);
    }

    // Signs the following requests with the account's key and URL.
    void use_account(const acme_account& account) {
        account_key_ = detail::load_private_key(account.private_key_pem_.view());
        account_url_ = account.account_url_;
    }

    template <typename runtime_type>
    ruvia::task<acme_order> create_order(runtime_type& context, const std::vector<std::string>& domains) {
        const auto* directory = co_await load_directory(context);
        const auto order_payload = build_order_payload(context, domains);
        const auto response = co_await signed_post(context, directory->new_order_, account_key(), account_url_,
            order_payload, acme_trace::new_order);
        const auto order_url = response.header("location");
        if (!order_url || order_url->empty()) {
            throw acme_error("ACME 订单响应缺少 Location", true);
        }
        (void)target_from_url(*order_url);
        auto order = parse_order(context, *order_url, response.body());
        if (order.authorizations_.empty() || order.finalize_.empty()) {
            throw acme_error("ACME 订单缺少验证或 Finalize 地址", true);
        }
        trace_.log(service::attempt_trace::log_level::info,
            "订单状态 " + order.status_ + "，包含 " + std::to_string(order.authorizations_.size()) + " 个授权");
        co_return order;
    }

    template <typename runtime_type>
    ruvia::task<acme_order> load_order(runtime_type& context, std::string_view order_url) {
        const auto response = co_await signed_post(context, order_url, account_key(), account_url_,
            std::string_view{}, acme_trace::order);
        auto order = parse_order(context, order_url, response.body());
        trace_.log(service::attempt_trace::log_level::info, "订单状态 " + order.status_);
        co_return order;
    }

    template <typename runtime_type>
    ruvia::task<acme_authorization> load_authorization(runtime_type& context, std::string_view url) {
        const auto response = co_await signed_post(context, url, account_key(), account_url_,
            std::string_view{}, acme_trace::authorization);
        co_return parse_authorization(context, url, response.body());
    }

    // The TXT record that answers the authorization's DNS-01 challenge.
    [[nodiscard]] dns_challenge_record challenge_record(const acme_authorization& authorization) const {
        if (!authorization.dns_challenge_) {
            throw acme_error("ACME 授权未提供 DNS-01 Challenge", true);
        }
        const auto key_authorization =
            authorization.dns_challenge_->token_ + "." + detail::jwk_thumbprint(account_key());
        return {.name_ = challenge_record_name(authorization.identifier_),
            .content_ = detail::base64_url(detail::sha256(key_authorization))};
    }

    // Tells the server the challenge is ready to be validated.
    template <typename runtime_type>
    ruvia::task<void> respond_challenge(runtime_type& context, std::string_view challenge_url) {
        (void)co_await signed_post(context, challenge_url, account_key(), account_url_, build_empty_payload(context),
            acme_trace::challenge);
    }

    // Polls the authorization until it is valid. Throws acme_pending when
    // it is still being validated at `deadline`.
    template <typename runtime_type>
    ruvia::task<void> wait_for_authorization(runtime_type& context, std::string_view url,
        std::chrono::steady_clock::time_point deadline) {
        while (true) {
            const auto authorization = co_await load_authorization(context, url);
            trace_.log(service::attempt_trace::log_level::info, "授权状态 " + authorization.status_);
            if (authorization.status_ == "valid") {
                co_return;
            }
            if (authorization.status_ == "invalid") {
                throw acme_error(trace_.redact_error(authorization.error_.empty()
                                                         ? "ACME DNS-01 验证失败"
                                                         : "ACME DNS-01 验证失败：" + authorization.error_),
                    false);
            }
            co_await pause_before_poll(context, deadline, "ACME DNS-01 验证仍在进行，下次重试时继续等待");
        }
    }

    template <typename runtime_type>
    ruvia::task<void> finalize_order(runtime_type& context, const acme_order& order, std::string_view csr) {
        acme_finalize_payload_output payload({.resource_ = context.pool()});
        payload.set<"csr">(csr);
        (void)co_await signed_post(context, order.finalize_, account_key(), account_url_,
            detail::json_string(std::move(payload), context.pool()), acme_trace::finalize);
    }

    // Polls the order until it is valid. Throws acme_pending when it is still
    // processing at `deadline`.
    template <typename runtime_type>
    ruvia::task<acme_order> wait_for_order(runtime_type& context, std::string_view order_url,
        std::chrono::steady_clock::time_point deadline) {
        while (true) {
            auto order = co_await load_order(context, order_url);
            if (order.status_ == "valid") {
                if (order.certificate_.empty()) {
                    throw acme_error("ACME 有效订单缺少证书下载地址", true);
                }
                co_return order;
            }
            if (order.status_ == "invalid") {
                throw acme_error(trace_.redact_error(order.error_.empty() ? "ACME 证书订单失败"
                                                                          : "ACME 证书订单失败：" + order.error_),
                    true);
            }
            co_await pause_before_poll(context, deadline, "ACME 证书签发仍在进行，下次重试时继续等待");
        }
    }

    // The PEM certificate chain of a valid order.
    template <typename runtime_type>
    ruvia::task<std::string> download_certificate(runtime_type& context, std::string_view certificate_url) {
        const auto response = co_await signed_post(context, certificate_url, account_key(), account_url_, "",
            acme_trace::certificate);
        if (response.body().empty()) {
            throw acme_error("ACME 返回的证书链为空", false);
        }
        co_return std::string(response.body().data(), response.body().size());
    }

private:
    static constexpr std::chrono::seconds poll_interval{2};

    [[nodiscard]] EVP_PKEY* account_key() const {
        if (!account_key_) {
            throw acme_error("ACME 账户尚未加载", true);
        }
        return account_key_.get();
    }

    template <typename runtime_type>
    ruvia::task<void> pause_before_poll(runtime_type& context, std::chrono::steady_clock::time_point deadline,
        std::string_view pending_message) const {
        if (std::chrono::steady_clock::now() + poll_interval >= deadline) {
            throw acme_pending(pending_message);
        }
        if (co_await ruvia::sleep_for(context.worker(), poll_interval, context.get_stop_token()) ==
            ruvia::timer_sleep_result::stop_requested) {
            throw acme_pending("证书签发已停止，下次重试时继续");
        }
    }

    template <typename runtime_type>
    ruvia::task<const detail::directory*> load_directory(runtime_type& context) {
        if (directory_) {
            co_return &*directory_;
        }
        const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
        const auto response = co_await send(context,
            {
                .method_ = "GET",
                .target_ = settings_.directory_target_,
                .headers_ = headers,
            },
            acme_trace::directory);
        if (!response.status().is_successful()) {
            throw_http_problem(context, response, "ACME Directory 查询失败");
        }
        const std::optional<acme_directory_input> parsed =
            ruvia::from_json<acme_directory_input>(response.body(), {.resource_ = context.pool()});
        if (!parsed) {
            throw acme_error("ACME Directory 响应不完整", false);
        }
        const auto& new_nonce = parsed->get<"new_nonce">();
        const auto& new_account = parsed->get<"new_account">();
        const auto& new_order = parsed->get<"new_order">();
        if (!new_nonce || !new_account || !new_order) {
            throw acme_error("ACME Directory 响应不完整", false);
        }
        detail::directory directory{
            std::string(new_nonce->view()),
            std::string(new_account->view()),
            std::string(new_order->view()),
        };
        (void)target_from_url(directory.new_nonce_);
        (void)target_from_url(directory.new_account_);
        (void)target_from_url(directory.new_order_);
        directory_ = std::move(directory);
        co_return &*directory_;
    }

    template <typename runtime_type>
    ruvia::task<std::string> fetch_nonce(runtime_type& context) {
        const auto* directory = co_await load_directory(context);
        const auto target = target_from_url(directory->new_nonce_);
        const auto response = co_await send(context, {.method_ = "HEAD", .target_ = target},
            acme_trace::new_nonce);
        if (!response.status().is_successful()) {
            throw_http_problem(context, response, "ACME Nonce 获取失败");
        }
        const auto nonce = response.header("replay-nonce");
        if (!nonce || nonce->empty()) {
            throw acme_error("ACME Nonce 响应缺少 Replay-Nonce", false);
        }
        co_return std::string(*nonce);
    }

    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response>
    signed_post(runtime_type& context, std::string_view url, EVP_PKEY* key,
        std::optional<std::string_view> account_url, std::string_view payload,
        const service::attempt_trace::outbound_policy& policy) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto nonce = co_await fetch_nonce(context);
            const auto body = build_jws(context, url, nonce, key, account_url, payload);
            const auto target = target_from_url(url);
            const std::array headers{
                ruvia::http_header_view{"content-type", "application/jose+json"},
                ruvia::http_header_view{"accept",
                    "application/json, application/pem-certificate-chain"},
            };
            auto response = co_await send(
                context, {
                             .method_ = "POST",
                             .target_ = target,
                             .headers_ = headers,
                             .content_ = ruvia::http_client_request_content_view::bytes(body),
                         },
                policy);
            if (response.status().is_successful()) {
                co_return response;
            }
            if (attempt == 0 && is_bad_nonce(context, response)) {
                trace_.log(service::attempt_trace::log_level::warning, "ACME 返回 badNonce，重新获取 Nonce 后重试");
                continue;
            }
            throw_http_problem(context, response, "ACME 请求失败");
        }
        throw acme_error("ACME Nonce 重试失败", false);
    }

    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response>
    send(runtime_type& context, const ruvia::http_client_request_view& request,
        const service::attempt_trace::outbound_policy& policy) const {
        co_return co_await service::outbound_http::send_buffered(context, scope_,
            {.origin_alias_ =
                    service::config::outbound_origin_alias(service::config::acme_origin(settings_.host_)),
                .policy_ = &policy},
            {.label_ = "ACME", .timeout_ = std::chrono::seconds(30)}, trace_, request);
    }

    template <typename runtime_type>
    std::string build_jws(runtime_type& context, std::string_view url, std::string_view nonce,
        EVP_PKEY* key, std::optional<std::string_view> account_url,
        std::string_view payload) const {
        std::string protected_json;
        if (account_url) {
            acme_protected_kid_output protected_value({.resource_ = context.pool()});
            protected_value.set<"alg">("RS256");
            protected_value.set<"kid">(*account_url);
            protected_value.set<"nonce">(nonce);
            protected_value.set<"url">(url);
            protected_json = detail::json_string(std::move(protected_value), context.pool());
        } else {
            const auto key_data = detail::get_jwk(key);
            acme_jwk_output jwk_value({.resource_ = context.pool()});
            jwk_value.set<"e">(key_data.exponent_);
            jwk_value.set<"kty">("RSA");
            jwk_value.set<"n">(key_data.modulus_);
            acme_protected_jwk_output protected_value({.resource_ = context.pool()});
            protected_value.set<"alg">("RS256");
            protected_value.set<"jwk">(std::move(jwk_value));
            protected_value.set<"nonce">(nonce);
            protected_value.set<"url">(url);
            protected_json = detail::json_string(std::move(protected_value), context.pool());
        }
        const auto protected_encoded = detail::base64_url(protected_json);
        const auto payload_encoded = detail::base64_url(payload);
        const auto signature = detail::sign(key, protected_encoded + "." + payload_encoded);
        acme_jws_output jws({.resource_ = context.pool()});
        jws.set<"protected_value">(protected_encoded);
        jws.set<"payload">(payload_encoded);
        jws.set<"signature">(signature);
        return detail::json_string(std::move(jws), context.pool());
    }

    template <typename runtime_type>
    acme_jws_output build_external_account_binding(runtime_type& context, EVP_PKEY* key,
        std::string_view new_account_url,
        const eab_credentials& credentials) const {
        const auto key_data = detail::get_jwk(key);
        acme_jwk_output account_jwk({.resource_ = context.pool()});
        account_jwk.set<"e">(key_data.exponent_);
        account_jwk.set<"kty">("RSA");
        account_jwk.set<"n">(key_data.modulus_);
        const auto payload = detail::json_string(std::move(account_jwk), context.pool());

        acme_eab_protected_output protected_value({.resource_ = context.pool()});
        protected_value.set<"alg">("HS256");
        protected_value.set<"kid">(credentials.key_id_);
        protected_value.set<"url">(new_account_url);
        const auto protected_json =
            detail::json_string(std::move(protected_value), context.pool());
        const auto protected_encoded = detail::base64_url(protected_json);
        const auto payload_encoded = detail::base64_url(payload);

        acme_jws_output result({.resource_ = context.pool()});
        result.set<"protected_value">(protected_encoded);
        result.set<"payload">(payload_encoded);
        result.set<"signature">(detail::hmac_sha256(credentials.hmac_key_.view(),
            protected_encoded + "." + payload_encoded));
        return result;
    }

    template <typename runtime_type>
    std::string build_order_payload(runtime_type& context, const std::vector<std::string>& domains) const {
        acme_order_payload_output payload({.resource_ = context.pool()});
        auto& identifiers = payload.ensure<"identifiers">();
        for (const auto& domain : domains) {
            auto& identifier =
                identifiers.emplace_back(ruvia::model_options{.resource_ = context.pool()});
            identifier.set<"type">("dns");
            identifier.set<"value">(domain);
        }
        return detail::json_string(std::move(payload), context.pool());
    }

    template <typename runtime_type>
    std::string build_empty_payload(runtime_type& context) const {
        acme_empty_payload_output payload({.resource_ = context.pool()});
        return detail::json_string(std::move(payload), context.pool());
    }

    template <typename runtime_type>
    acme_order parse_order(runtime_type& context, std::string_view url, std::string_view body) const {
        const std::optional<acme_order_input> parsed =
            ruvia::from_json<acme_order_input>(body, {.resource_ = context.pool()});
        if (!parsed) {
            throw acme_error("ACME 订单响应格式无效", false);
        }
        acme_order result;
        result.url_ = std::string(url);
        result.status_ = detail::optional_string(parsed->get<"status">());
        result.finalize_ = detail::optional_string(parsed->get<"finalize">());
        result.certificate_ = detail::optional_string(parsed->get<"certificate">());
        result.error_ = detail::problem_detail(parsed->get<"error">());
        if (const auto& values = parsed->get<"authorizations">(); values) {
            result.authorizations_.reserve(values->size());
            for (const auto& value : *values) {
                result.authorizations_.emplace_back(value.view());
            }
        }
        return result;
    }

    template <typename runtime_type>
    acme_authorization parse_authorization(runtime_type& context, std::string_view url, std::string_view body) const {
        const std::optional<acme_authorization_input> parsed =
            ruvia::from_json<acme_authorization_input>(body, {.resource_ = context.pool()});
        if (!parsed) {
            throw acme_error("ACME 授权响应格式无效", false);
        }
        acme_authorization result;
        result.url_ = std::string(url);
        result.status_ = detail::optional_string(parsed->get<"status">());
        if (const auto& identifier = parsed->get<"identifier">(); identifier) {
            result.identifier_ = detail::optional_string(identifier->get<"value">());
        }
        if (const auto& challenges = parsed->get<"challenges">(); challenges) {
            for (const auto& challenge : *challenges) {
                const auto type = detail::optional_string(challenge.get<"type">());
                if (type != "dns-01") {
                    continue;
                }
                const auto challenge_url = detail::optional_string(challenge.get<"url">());
                const auto token = detail::optional_string(challenge.get<"token">());
                if (!challenge_url.empty() && !token.empty()) {
                    result.dns_challenge_ = detail::challenge{challenge_url, token};
                }
                result.error_ = detail::problem_detail(challenge.get<"error">());
                break;
            }
        }
        return result;
    }

    [[nodiscard]] std::string target_from_url(std::string_view url) const {
        const auto prefix = "https://" + settings_.host_;
        if (!url.starts_with(prefix)) {
            throw acme_error("ACME 响应返回了未配置的服务地址", true);
        }
        const auto target = url.substr(prefix.size());
        if (target.empty() || target.front() != '/') {
            throw acme_error("ACME 服务地址格式无效", true);
        }
        return std::string(target);
    }

    static std::string challenge_record_name(std::string_view identifier) {
        if (identifier.starts_with("*.")) {
            identifier.remove_prefix(2);
        }
        if (identifier.empty()) {
            throw acme_error("ACME 授权缺少域名标识", true);
        }
        return "_acme-challenge." + std::string(identifier);
    }

    template <typename runtime_type>
    static bool is_bad_nonce(runtime_type& context,
        const service::outbound_http::buffered_response& response) {
        const std::optional<acme_problem_input> parsed =
            ruvia::from_json<acme_problem_input>(response.body(), {.resource_ = context.pool()});
        if (!parsed) {
            return false;
        }
        const auto& type = parsed->get<"type">();
        return type && type->view().ends_with(":badNonce");
    }

    template <typename runtime_type>
    [[noreturn]] void throw_http_problem(runtime_type& context, const service::outbound_http::buffered_response& response,
        std::string_view fallback) const {
        throw detail::acme_problem_error(trace_, response.body(), response.status(), fallback, context.pool());
    }

    acme_settings settings_;
    service::attempt_trace::recorder& trace_;
    service::outbound_http::quota_scope scope_;
    std::optional<detail::directory> directory_;
    detail::evp_pkey_ptr account_key_{nullptr, EVP_PKEY_free};
    std::string account_url_;
};

}  // namespace service::certificate_issuance
