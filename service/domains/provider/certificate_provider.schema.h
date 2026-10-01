#pragma once

#include <string_view>

#include <utility>

#include <ruvia/web/Controller.h>
#include <ruvia/web/Validation.h>

#include "service/domains/provider/certificate_provider.types.h"

namespace service::provider {

inline bool isCertificateProvider(std::string_view value) noexcept {
    return value == "letsencrypt" || value == "zerossl";
}
inline bool isCertificateCredentialMode(std::string_view value) noexcept {
    return value == "email" || value == "access_key";
}
inline bool isProviderEmail(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 == value.size() ||
        value.find('@', at + 1) != std::string_view::npos || value.back() == '.') return false;
    const auto local = value.substr(0, at);
    const auto domain = value.substr(at + 1);
    constexpr std::string_view punctuation = ".!#$%&'*+/=?^_`{|}~-";
    for (const unsigned char ch : local) {
        const bool alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9');
        if (!alnum && punctuation.find(static_cast<char>(ch)) == std::string_view::npos) return false;
    }
    const auto alnum = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    };
    bool hasDot = false;
    std::size_t start = 0;
    while (start < domain.size()) {
        const auto end = domain.find('.', start);
        const auto label = domain.substr(start, end == std::string_view::npos
                                                   ? domain.size() - start : end - start);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) || !alnum(label.back()))
            return false;
        for (const unsigned char ch : label) if (!alnum(ch) && ch != '-') return false;
        if (end == std::string_view::npos) break;
        hasDot = true;
        start = end + 1;
    }
    return hasDot;
}
inline bool isNonWhitespaceCredential(std::string_view value) noexcept {
    return value.find_first_of(" \t\n\r\f\v") == std::string_view::npos;
}

class CreateCertificateProviderValidator final : public ruvia::Middleware {
  public:
    void validate(const CreateCertificateProviderBody& body, ruvia::Validator& validator) const {
        const auto& provider = body.get<"provider">();
        validator.required(provider, "provider", "请选择供应商类型");
        if (provider && !isCertificateProvider(provider->view()))
            validator.add("provider", "regex", "供应商类型不正确");

        const auto& mode = body.get<"credentialMode">();
        validator.required(mode, "credential_mode", "请选择接入方式");
        if (mode && !isCertificateCredentialMode(mode->view()))
            validator.add("credential_mode", "regex", "接入方式不正确");

        const auto& email = body.get<"accountEmail">();
        validator.maxLength(email, "account_email", 254, "账户邮箱最多254个字符");
        if (email && !isProviderEmail(email->view()))
            validator.add("account_email", "regex", "账户邮箱格式不正确");

        const auto& key = body.get<"accessKey">();
        validator.minLength(key, "access_key", 1, "API Access Key 不能为空");
        validator.maxLength(key, "access_key", 255, "API Access Key 最多255个字符");
        if (key && !isNonWhitespaceCredential(key->view()))
            validator.add("access_key", "regex", "API Access Key 格式不正确");
    }

    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validated<CreateCertificateProviderBody>();
        ruvia::Validator validator({.resource = context.pool()});
        validate(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

class UpdateCertificateProviderValidator final : public ruvia::Middleware {
  public:
    void validate(const UpdateCertificateProviderBody& body, ruvia::Validator& validator) const {
        const auto& mode = body.get<"credentialMode">();
        validator.required(mode, "credential_mode", "请选择接入方式");
        if (mode && !isCertificateCredentialMode(mode->view()))
            validator.add("credential_mode", "regex", "接入方式不正确");

        const auto& email = body.get<"accountEmail">();
        validator.maxLength(email, "account_email", 254, "账户邮箱最多254个字符");
        if (email && !isProviderEmail(email->view()))
            validator.add("account_email", "regex", "账户邮箱格式不正确");

        const auto& key = body.get<"accessKey">();
        validator.minLength(key, "access_key", 1, "API Access Key 不能为空");
        validator.maxLength(key, "access_key", 255, "API Access Key 最多255个字符");
        if (key && !isNonWhitespaceCredential(key->view()))
            validator.add("access_key", "regex", "API Access Key 格式不正确");
    }

    ruvia::Task<void> handle(ruvia::Context& context, ruvia::Next& next) {
        const auto& body = context.req().validated<UpdateCertificateProviderBody>();
        ruvia::Validator validator({.resource = context.pool()});
        validate(body, validator);
        std::move(validator).throwIfInvalid();
        co_await next();
    }
};

} // namespace service::provider
