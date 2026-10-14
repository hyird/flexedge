#pragma once

#include <ruvia/web/controller.h>
#include <ruvia/web/validation.h>

#include <string_view>
#include <utility>

#include "service/domains/provider/dns_provider_types.h"

namespace service::provider {

inline bool has_non_whitespace_dns_name(std::string_view value) noexcept {
    constexpr std::string_view line_breaks = "\r\n";
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    return value.find_first_of(line_breaks) == std::string_view::npos &&
           value.find_first_not_of(whitespace) != std::string_view::npos;
}
inline bool is_supported_dns_provider(std::string_view value) noexcept {
    return value == "cloudflare" || value == "aliyun";
}
inline bool is_non_whitespace_dns_credential(std::string_view value) noexcept {
    return value.find_first_of(" \t\n\r\f\v") == std::string_view::npos;
}

class create_dns_provider_validator final {
public:
    void validate(const create_dns_provider_body& body, ruvia::validator& validator) const {
        const auto& name = body.get<"name">();
        validator.required(name, "name", "账号名称不能为空");
        validator.min_length(name, "name", 1, "账号名称不能为空");
        validator.max_length(name, "name", 100, "账号名称最多100个字符");
        if (name && !has_non_whitespace_dns_name(name->view())) {
            validator.add("name", "regex", "账号名称不能为空");
        }

        const auto& provider = body.get<"provider">();
        validator.required(provider, "provider", "服务商类型不能为空");
        if (provider && !is_supported_dns_provider(provider->view())) {
            validator.add("provider", "regex", "服务商类型不支持");
        }

        const auto& account = body.get<"account_id">();
        validator.required(account, "account_id", "账户标识不能为空");
        validator.min_length(account, "account_id", 8, "账户标识格式不正确");
        validator.max_length(account, "account_id", 128, "账户标识格式不正确");
        if (account && !is_non_whitespace_dns_credential(account->view())) {
            validator.add("account_id", "regex", "账户标识格式不正确");
        }

        const auto& token = body.get<"api_token">();
        validator.required(token, "api_token", "访问密钥不能为空");
        validator.min_length(token, "api_token", 16, "访问密钥格式不正确");
        validator.max_length(token, "api_token", 256, "访问密钥格式不正确");
    }

    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated<create_dns_provider_body>();
        ruvia::validator validator({.resource_ = context.pool()});
        validate(body, validator);
        validator.throw_if_invalid();
        co_await next();
    }
};

class update_dns_provider_validator final {
public:
    void validate(const update_dns_provider_body& body, ruvia::validator& validator) const {
        const auto& name = body.get<"name">();
        validator.required(name, "name", "账号名称不能为空");
        validator.min_length(name, "name", 1, "账号名称不能为空");
        validator.max_length(name, "name", 100, "账号名称最多100个字符");
        if (name && !has_non_whitespace_dns_name(name->view())) {
            validator.add("name", "regex", "账号名称不能为空");
        }

        const auto& token = body.get<"api_token">();
        validator.min_length(token, "api_token", 16, "访问密钥格式不正确");
        validator.max_length(token, "api_token", 256, "访问密钥格式不正确");
    }

    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated<update_dns_provider_body>();
        ruvia::validator validator({.resource_ = context.pool()});
        validate(body, validator);
        validator.throw_if_invalid();
        co_await next();
    }
};

}  // namespace service::provider
