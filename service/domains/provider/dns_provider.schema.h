#pragma once

#include <string_view>

#include <utility>

#include <ruvia/web/Controller.h>
#include <ruvia/web/Validation.h>

#include "service/domains/provider/dns_provider.types.h"

namespace service::provider {

inline bool hasNonWhitespaceDnsName(std::string_view value) noexcept {
    constexpr std::string_view lineBreaks = "\r\n";
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    return value.find_first_of(lineBreaks) == std::string_view::npos &&
           value.find_first_not_of(whitespace) != std::string_view::npos;
}
inline bool isSupportedDnsProvider(std::string_view value) noexcept {
    return value == "cloudflare" || value == "aliyun";
}
inline bool isNonWhitespaceDnsCredential(std::string_view value) noexcept {
    return value.find_first_of(" \t\n\r\f\v") == std::string_view::npos;
}

class CreateDnsProviderValidator final : public ruvia::Middleware {
  public:
    void validate(const CreateDnsProviderBody& body, ruvia::Validator& validator) const {
        const auto& name = body.get<"name">();
        validator.required(name, "name", "账号名称不能为空");
        validator.minLength(name, "name", 1, "账号名称不能为空");
        validator.maxLength(name, "name", 100, "账号名称最多100个字符");
        if (name && !hasNonWhitespaceDnsName(name->view()))
            validator.add("name", "regex", "账号名称不能为空");

        const auto& provider = body.get<"provider">();
        validator.required(provider, "provider", "服务商类型不能为空");
        if (provider && !isSupportedDnsProvider(provider->view()))
            validator.add("provider", "regex", "服务商类型不支持");

        const auto& account = body.get<"accountId">();
        validator.required(account, "account_id", "账户标识不能为空");
        validator.minLength(account, "account_id", 8, "账户标识格式不正确");
        validator.maxLength(account, "account_id", 128, "账户标识格式不正确");
        if (account && !isNonWhitespaceDnsCredential(account->view()))
            validator.add("account_id", "regex", "账户标识格式不正确");

        const auto& token = body.get<"apiToken">();
        validator.required(token, "api_token", "访问密钥不能为空");
        validator.minLength(token, "api_token", 16, "访问密钥格式不正确");
        validator.maxLength(token, "api_token", 256, "访问密钥格式不正确");
    }

    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validated<CreateDnsProviderBody>();
        ruvia::Validator validator({.resource = context.pool()});
        validate(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

class UpdateDnsProviderValidator final : public ruvia::Middleware {
  public:
    void validate(const UpdateDnsProviderBody& body, ruvia::Validator& validator) const {
        const auto& name = body.get<"name">();
        validator.required(name, "name", "账号名称不能为空");
        validator.minLength(name, "name", 1, "账号名称不能为空");
        validator.maxLength(name, "name", 100, "账号名称最多100个字符");
        if (name && !hasNonWhitespaceDnsName(name->view()))
            validator.add("name", "regex", "账号名称不能为空");

        const auto& token = body.get<"apiToken">();
        validator.minLength(token, "api_token", 16, "访问密钥格式不正确");
        validator.maxLength(token, "api_token", 256, "访问密钥格式不正确");
    }

    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validated<UpdateDnsProviderBody>();
        ruvia::Validator validator({.resource = context.pool()});
        validate(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

} // namespace service::provider
