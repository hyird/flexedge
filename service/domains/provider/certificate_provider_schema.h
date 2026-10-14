#pragma once

#include <ruvia/web/controller.h>
#include <ruvia/web/validation.h>

#include <string_view>
#include <utility>

#include "service/domains/provider/certificate_provider_types.h"

namespace service::provider {

inline bool is_certificate_provider(std::string_view value) noexcept {
    return value == "letsencrypt" || value == "zerossl";
}
inline bool is_certificate_credential_mode(std::string_view value) noexcept {
    return value == "email" || value == "access_key";
}
inline bool is_provider_email(std::string_view value) noexcept {
    const auto at = value.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 == value.size() ||
        value.find('@', at + 1) != std::string_view::npos || value.back() == '.') {
        return false;
    }
    const auto local = value.substr(0, at);
    const auto domain = value.substr(at + 1);
    constexpr std::string_view punctuation = ".!#$%&'*+/=?^_`{|}~-";
    for (const unsigned char ch : local) {
        const bool alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                           (ch >= '0' && ch <= '9');
        if (!alnum && punctuation.find(static_cast<char>(ch)) == std::string_view::npos) {
            return false;
        }
    }
    const auto alnum = [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
    };
    bool has_dot = false;
    std::size_t start = 0;
    while (start < domain.size()) {
        const auto end = domain.find('.', start);
        const auto label = domain.substr(start, end == std::string_view::npos
                                                    ? domain.size() - start
                                                    : end - start);
        if (label.empty() || label.size() > 63 || !alnum(label.front()) || !alnum(label.back())) {
            return false;
        }
        for (const unsigned char ch : label) {
            if (!alnum(ch) && ch != '-') {
                return false;
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        has_dot = true;
        start = end + 1;
    }
    return has_dot;
}
inline bool is_non_whitespace_credential(std::string_view value) noexcept {
    return value.find_first_of(" \t\n\r\f\v") == std::string_view::npos;
}

class create_certificate_provider_validator final {
public:
    void validate(const create_certificate_provider_body& body, ruvia::validator& validator) const {
        const auto& provider = body.get<"provider">();
        validator.required(provider, "provider", "请选择供应商类型");
        if (provider && !is_certificate_provider(provider->view())) {
            validator.add("provider", "regex", "供应商类型不正确");
        }

        const auto& mode = body.get<"credential_mode">();
        validator.required(mode, "credential_mode", "请选择接入方式");
        if (mode && !is_certificate_credential_mode(mode->view())) {
            validator.add("credential_mode", "regex", "接入方式不正确");
        }

        const auto& email = body.get<"account_email">();
        validator.max_length(email, "account_email", 254, "账户邮箱最多254个字符");
        if (email && !is_provider_email(email->view())) {
            validator.add("account_email", "regex", "账户邮箱格式不正确");
        }

        const auto& key = body.get<"access_key">();
        validator.min_length(key, "access_key", 1, "API Access Key 不能为空");
        validator.max_length(key, "access_key", 255, "API Access Key 最多255个字符");
        if (key && !is_non_whitespace_credential(key->view())) {
            validator.add("access_key", "regex", "API Access Key 格式不正确");
        }
    }

    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated<create_certificate_provider_body>();
        ruvia::validator validator({.resource_ = context.pool()});
        validate(body, validator);
        validator.throw_if_invalid();
        co_await next();
    }
};

class update_certificate_provider_validator final {
public:
    void validate(const update_certificate_provider_body& body, ruvia::validator& validator) const {
        const auto& mode = body.get<"credential_mode">();
        validator.required(mode, "credential_mode", "请选择接入方式");
        if (mode && !is_certificate_credential_mode(mode->view())) {
            validator.add("credential_mode", "regex", "接入方式不正确");
        }

        const auto& email = body.get<"account_email">();
        validator.max_length(email, "account_email", 254, "账户邮箱最多254个字符");
        if (email && !is_provider_email(email->view())) {
            validator.add("account_email", "regex", "账户邮箱格式不正确");
        }

        const auto& key = body.get<"access_key">();
        validator.min_length(key, "access_key", 1, "API Access Key 不能为空");
        validator.max_length(key, "access_key", 255, "API Access Key 最多255个字符");
        if (key && !is_non_whitespace_credential(key->view())) {
            validator.add("access_key", "regex", "API Access Key 格式不正确");
        }
    }

    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated<update_certificate_provider_body>();
        ruvia::validator validator({.resource_ = context.pool()});
        validate(body, validator);
        validator.throw_if_invalid();
        co_await next();
    }
};

}  // namespace service::provider
