#pragma once

#include <ruvia/web/controller.h>
#include <ruvia/web/validation.h>

#include <algorithm>
#include <string_view>
#include <utility>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/domains/provider/certificate_provider_types.h"
#include "service/utils/text.h"

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
    auto domain = value.substr(at + 1);
    constexpr std::string_view punctuation = ".!#$%&'*+/=?^_`{|}~-";
    for (const unsigned char ch : local) {
        if (!service::utils::is_ascii_alnum(static_cast<char>(ch)) &&
            punctuation.find(static_cast<char>(ch)) == std::string_view::npos) {
            return false;
        }
    }
    if (service::common::is_hostname(domain, false)) {
        return true;
    }
    bool has_dot = false;
    while (!domain.empty()) {
        const auto dot = domain.find('.');
        if (!service::common::is_valid_dns_label(domain.substr(0, dot), false)) {
            return false;
        }
        if (dot == std::string_view::npos) {
            return has_dot;
        }
        has_dot = true;
        domain.remove_prefix(dot + 1);
    }
    return false;
}
inline bool is_non_whitespace_credential(std::string_view value) noexcept {
    return std::ranges::none_of(value, service::utils::is_whitespace_ascii);
}

inline void validate_create_certificate_provider(
    const create_certificate_provider_body& body, ruvia::validator& validator) {
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

inline void validate_update_certificate_provider(
    const update_certificate_provider_body& body, ruvia::validator& validator) {
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

using create_certificate_provider_validator = service::common::json_validator_middleware<
    create_certificate_provider_body, validate_create_certificate_provider>;
using update_certificate_provider_validator = service::common::json_validator_middleware<
    update_certificate_provider_body, validate_update_certificate_provider>;

}  // namespace service::provider
