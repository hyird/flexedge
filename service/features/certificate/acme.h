#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/http_client_handle.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/web/model.h"

#include "service/config/outbound.h"
#include "service/features/certificate/acme_jws.h"
#include "service/features/certificate/acme_transport.h"
#include "service/features/certificate/acme_types.h"
#include "service/features/certificate/certificate_inspection.h"
#include "service/features/certificate/certificate_request.h"
#include "service/features/certificate/dns_challenge.h"
#include "service/features/outbound_http/client.h"
#include "service/features/sync_runtime/state.h"

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

struct authorization final {
    std::string status_;
    std::string identifier_;
    std::optional<challenge> dns_challenge_;
    std::string error_;
};

struct order final {
    std::string status_;
    std::vector<std::string> authorizations_;
    std::string finalize_;
    std::string certificate_;
    std::string error_;
};

struct pending_authorization final {
    std::string authorization_url_;
    std::string challenge_url_;
};

struct authorization_plan final {
    std::vector<pending_authorization> pending_;
    std::vector<dns_challenge_spec> challenge_specs_;
};

struct order_session final {
    directory directory_;
    std::string url_;
    order order_;
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

}  // namespace detail

class acme_client final {
public:
    explicit acme_client(acme_settings settings)
        : settings_(std::move(settings)) {}

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
        const std::optional<eab_credentials>& eab) const {
        auto key = detail::load_private_key(private_key_pem);
        const auto directory = co_await load_directory(context);
        acme_account_payload_output payload({.resource_ = context.pool()});
        payload.set<"terms_agreed">(true);
        if (account_email && !account_email->empty()) {
            const auto contact = "mailto:" + std::string(*account_email);
            payload.ensure<"contact">().emplace_back(
                contact, ruvia::model_options{.resource_ = context.pool()});
        }
        if (eab) {
            payload.set<"external_account_binding">(
                build_external_account_binding(context, key.get(), directory.new_account_, *eab));
        }
        const auto response =
            co_await signed_post(context, directory, directory.new_account_, key.get(), std::nullopt,
                detail::json_string(std::move(payload), context.pool()));
        const auto location = response.header("location");
        if (!location || location->empty()) {
            throw acme_error("ACME 账户响应缺少 Location", true);
        }
        (void)target_from_url(*location);
        co_return std::string(*location);
    }

    template <typename runtime_type>
    ruvia::task<issued_certificate>
    issue(runtime_type& context, std::string_view account_private_key_pem, std::string_view account_url,
        const std::vector<std::string>& domains, const dns_challenge_config_view& dns,
        const service::sync_runtime::running_marker_lease& certificate_task) const {
        auto account_key = detail::load_private_key(account_private_key_pem);
        const auto order_session =
            co_await create_order(context, account_key.get(), account_url, domains);
        const auto authorization_plan =
            co_await collect_authorization_plan(context, order_session, account_key.get(), account_url);
        bool challenges_persisted = false;
        std::exception_ptr validation_failure;
        try {
            co_await validate_authorizations(context, order_session, authorization_plan,
                account_key.get(), account_url, dns, certificate_task,
                challenges_persisted);
        } catch (...) {
            validation_failure = std::current_exception();
        }
        co_await cleanup_challenges(context, dns, certificate_task, challenges_persisted);
        if (validation_failure) {
            std::rethrow_exception(validation_failure);
        }
        co_return co_await finalize_order(context, order_session, account_key.get(), account_url,
            domains, certificate_task);
    }

private:
    template <typename runtime_type>
    ruvia::task<detail::order_session> create_order(runtime_type& context, EVP_PKEY* account_key,
        std::string_view account_url,
        const std::vector<std::string>& domains) const {
        auto directory = co_await load_directory(context);
        const auto order_payload = build_order_payload(context, domains);
        const auto order_response = co_await signed_post(context, directory, directory.new_order_,
            account_key, account_url, order_payload);
        const auto order_url_header = order_response.header("location");
        if (!order_url_header || order_url_header->empty()) {
            throw acme_error("ACME 订单响应缺少 Location", true);
        }
        std::string order_url(*order_url_header);
        (void)target_from_url(order_url);
        auto order = parse_order(context, order_response.body());
        if (order.authorizations_.empty() || order.finalize_.empty()) {
            throw acme_error("ACME 订单缺少验证或 Finalize 地址", true);
        }
        co_return detail::order_session{.directory_ = std::move(directory),
            .url_ = std::move(order_url),
            .order_ = std::move(order)};
    }

    template <typename runtime_type>
    ruvia::task<detail::authorization_plan>
    collect_authorization_plan(runtime_type& context, const detail::order_session& order_session,
        EVP_PKEY* account_key, std::string_view account_url) const {
        detail::authorization_plan plan;
        const auto thumbprint = detail::jwk_thumbprint(account_key);
        for (const auto& authorization_url : order_session.order_.authorizations_) {
            auto authorization = co_await load_authorization(
                context, order_session.directory_, authorization_url, account_key, account_url);
            if (authorization.status_ == "valid") {
                continue;
            }
            if (authorization.status_ == "invalid") {
                throw acme_error(authorization.error_.empty()
                                     ? "ACME 授权已失效"
                                     : "ACME 授权失败：" + authorization.error_,
                    false);
            }
            if (!authorization.dns_challenge_) {
                throw acme_error("ACME 授权未提供 DNS-01 Challenge", true);
            }
            const auto key_authorization = authorization.dns_challenge_->token_ + "." + thumbprint;
            const auto digest = detail::sha256(key_authorization);
            const auto content = detail::base64_url(digest);
            const auto record_name = challenge_record_name(authorization.identifier_);
            plan.challenge_specs_.push_back({.name_ = record_name, .content_ = content});
            plan.pending_.push_back(detail::pending_authorization{
                .authorization_url_ = authorization_url,
                .challenge_url_ = authorization.dns_challenge_->url_,
            });
        }
        co_return plan;
    }

    template <typename runtime_type>
    ruvia::task<void>
    validate_authorizations(runtime_type& context, const detail::order_session& order_session,
        const detail::authorization_plan& plan, EVP_PKEY* account_key,
        std::string_view account_url, const dns_challenge_config_view& dns,
        const service::sync_runtime::running_marker_lease& certificate_task,
        bool& challenges_persisted) const {
        if (!plan.pending_.empty()) {
            const auto dns_task_id =
                co_await persist_dns_challenges(context, certificate_task, dns, plan.challenge_specs_);
            challenges_persisted = true;
            co_await wait_for_dns_marker(context, dns_task_id, certificate_task);
            if (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(10),
                    context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                throw acme_error("证书签发已停止", false);
            }
            const auto empty_payload = build_empty_payload(context);
            for (const auto& pending : plan.pending_) {
                (void)co_await signed_post(context, order_session.directory_, pending.challenge_url_,
                    account_key, account_url, empty_payload);
            }
            for (const auto& pending : plan.pending_) {
                co_await wait_for_authorization(context, order_session.directory_,
                    pending.authorization_url_, account_key, account_url,
                    certificate_task);
            }
        }
        co_return;
    }

    template <typename runtime_type>
    ruvia::task<void>
    cleanup_challenges(runtime_type& context, const dns_challenge_config_view& dns,
        const service::sync_runtime::running_marker_lease& certificate_task,
        bool challenges_persisted) const {
        if (!challenges_persisted) {
            co_return;
        }
        std::exception_ptr cleanup_failure;
        try {
            const auto dns_task_id = co_await retire_dns_challenges(context, dns.dns_zone_id_,
                dns.certificate_id_, certificate_task);
            if (!dns_task_id.empty()) {
                co_await wait_for_dns_marker(context, dns_task_id, certificate_task);
            }
        } catch (...) {
            cleanup_failure = std::current_exception();
        }
        if (cleanup_failure) {
            std::rethrow_exception(cleanup_failure);
        }
        co_return;
    }

    template <typename runtime_type>
    ruvia::task<issued_certificate>
    finalize_order(runtime_type& context, const detail::order_session& order_session, EVP_PKEY* account_key,
        std::string_view account_url, const std::vector<std::string>& domains,
        const service::sync_runtime::running_marker_lease& certificate_task) const {
        auto certificate_request = detail::create_certificate_request(domains);
        acme_finalize_payload_output finalize_payload({.resource_ = context.pool()});
        finalize_payload.set<"csr">(certificate_request.csr_);
        (void)co_await signed_post(
            context, order_session.directory_, order_session.order_.finalize_, account_key, account_url,
            detail::json_string(std::move(finalize_payload), context.pool()));
        const auto order = co_await wait_for_order(context, order_session.directory_, order_session.url_,
            account_key, account_url, certificate_task);
        if (order.certificate_.empty()) {
            throw acme_error("ACME 有效订单缺少证书下载地址", true);
        }
        const auto certificate_response = co_await signed_post(
            context, order_session.directory_, order.certificate_, account_key, account_url, "");
        if (certificate_response.body().empty()) {
            throw acme_error("ACME 返回的证书链为空", false);
        }
        co_return detail::inspect_certificate(
            std::move(certificate_request.private_key_pem_),
            std::string(certificate_response.body().data(), certificate_response.body().size()),
            domains);
    }

    template <typename runtime_type>
    ruvia::task<detail::directory> load_directory(runtime_type& context) const {
        const std::array headers{ruvia::http_header_view{"accept", "application/json"}};
        const auto response = co_await send(context, {
                                                         .method_ = "GET",
                                                         .target_ = settings_.directory_target_,
                                                         .headers_ = headers,
                                                     });
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
        co_return directory;
    }

    template <typename runtime_type>
    ruvia::task<std::string> fetch_nonce(runtime_type& context,
        const detail::directory& directory) const {
        const auto target = target_from_url(directory.new_nonce_);
        const auto response = co_await send(context, {.method_ = "HEAD", .target_ = target});
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
    signed_post(runtime_type& context, const detail::directory& directory, std::string_view url,
        EVP_PKEY* key, std::optional<std::string_view> account_url,
        std::string_view payload) const {
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto nonce = co_await fetch_nonce(context, directory);
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
                         });
            if (response.status().is_successful()) {
                co_return response;
            }
            if (attempt == 0 && is_bad_nonce(context, response)) {
                continue;
            }
            throw_http_problem(context, response, "ACME 请求失败");
        }
        throw acme_error("ACME Nonce 重试失败", false);
    }

    template <typename runtime_type>
    ruvia::task<service::outbound_http::buffered_response>
    send(runtime_type& context, const ruvia::http_client_request_view& request) const {
        try {
            auto&& client = context.get_http_client(
                service::config::outbound_origin_alias(service::config::acme_origin(settings_.host_)));
            co_return co_await service::outbound_http::send_buffered(
                client, request,
                {.timeout_ = std::chrono::seconds(30), .stop_token_ = context.get_stop_token()});
        } catch (const ruvia::http_client_error& error) {
            using code = ruvia::http_client_error::code_type;
            switch (error.code()) {
                case code::timeout:
                    throw acme_error("ACME 请求超时（30 秒）", false);
                case code::resolve_failed:
                    throw acme_error("ACME 服务域名解析失败", false);
                case code::connect_failed:
                    throw acme_error("ACME 服务连接失败", false);
                case code::tls_failed:
                    throw acme_error("ACME TLS 连接失败", false);
                default:
                    throw acme_error("ACME HTTP 客户端失败", false);
            }
        }
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
    detail::order parse_order(runtime_type& context, std::string_view body) const {
        const std::optional<acme_order_input> parsed =
            ruvia::from_json<acme_order_input>(body, {.resource_ = context.pool()});
        if (!parsed) {
            throw acme_error("ACME 订单响应格式无效", false);
        }
        detail::order result;
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
    detail::authorization parse_authorization(runtime_type& context, std::string_view body) const {
        const std::optional<acme_authorization_input> parsed =
            ruvia::from_json<acme_authorization_input>(body, {.resource_ = context.pool()});
        if (!parsed) {
            throw acme_error("ACME 授权响应格式无效", false);
        }
        detail::authorization result;
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
                const auto url = detail::optional_string(challenge.get<"url">());
                const auto token = detail::optional_string(challenge.get<"token">());
                if (!url.empty() && !token.empty()) {
                    result.dns_challenge_ = detail::challenge{url, token};
                }
                result.error_ = detail::problem_detail(challenge.get<"error">());
                break;
            }
        }
        return result;
    }

    template <typename runtime_type>
    ruvia::task<detail::authorization>
    load_authorization(runtime_type& context, const detail::directory& directory, std::string_view url,
        EVP_PKEY* key, std::string_view account_url) const {
        const auto response =
            co_await signed_post(context, directory, url, key, account_url, std::string_view{});
        co_return parse_authorization(context, response.body());
    }

    template <typename runtime_type>
    ruvia::task<void>
    wait_for_authorization(runtime_type& context, const detail::directory& directory,
        std::string_view authorization_url, EVP_PKEY* key,
        std::string_view account_url,
        const service::sync_runtime::running_marker_lease& certificate_task) const {
        auto db = context.db();
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (!co_await service::sync_runtime::renew_running_lease(db, certificate_task)) {
                throw std::runtime_error("证书任务 lease 已失效");
            }
            const auto authorization =
                co_await load_authorization(context, directory, authorization_url, key, account_url);
            if (authorization.status_ == "valid") {
                co_return;
            }
            if (authorization.status_ == "invalid") {
                throw acme_error(authorization.error_.empty()
                                     ? "ACME DNS-01 验证失败"
                                     : "ACME DNS-01 验证失败：" + authorization.error_,
                    false);
            }
            if (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(2),
                    context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                throw acme_error("证书签发已停止", false);
            }
        }
        throw acme_error("ACME DNS-01 验证等待超时", false);
    }

    template <typename runtime_type>
    ruvia::task<detail::order>
    wait_for_order(runtime_type& context, const detail::directory& directory, std::string_view order_url,
        EVP_PKEY* key, std::string_view account_url,
        const service::sync_runtime::running_marker_lease& certificate_task) const {
        auto db = context.db();
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (!co_await service::sync_runtime::renew_running_lease(db, certificate_task)) {
                throw std::runtime_error("证书任务 lease 已失效");
            }
            const auto response = co_await signed_post(context, directory, order_url, key, account_url,
                std::string_view{});
            auto order = parse_order(context, response.body());
            if (order.status_ == "valid") {
                co_return order;
            }
            if (order.status_ == "invalid") {
                throw acme_error(order.error_.empty() ? "ACME 证书订单失败"
                                                      : "ACME 证书订单失败：" + order.error_,
                    true);
            }
            if (co_await ruvia::sleep_for(context.worker(), std::chrono::seconds(2),
                    context.get_stop_token()) ==
                ruvia::timer_sleep_result::stop_requested) {
                throw acme_error("证书签发已停止", false);
            }
        }
        throw acme_error("ACME 证书签发等待超时", false);
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

    static bool permanent_problem(std::string_view type, ruvia::http_status_code status) {
        if (status.value() == 429 || status.is_server_error()) {
            return false;
        }
        return type.ends_with(":malformed") || type.ends_with(":unauthorized") ||
               type.ends_with(":rejectedIdentifier") || type.ends_with(":accountDoesNotExist");
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
    [[noreturn]] static void
    throw_http_problem(runtime_type& context, const service::outbound_http::buffered_response& response,
        std::string_view fallback) {
        const std::optional<acme_problem_input> parsed =
            ruvia::from_json<acme_problem_input>(response.body(), {.resource_ = context.pool()});
        std::string type;
        std::string message(fallback);
        if (parsed) {
            type = detail::optional_string(parsed->get<"type">());
            const auto detail_value = detail::optional_string(parsed->get<"detail">());
            if (!detail_value.empty()) {
                message.append("：").append(detail_value);
            }
        }
        throw acme_error(std::move(message), permanent_problem(type, response.status()));
    }

    acme_settings settings_;
};

}  // namespace service::certificate_issuance
