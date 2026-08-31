#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/provider/certificate_provider.types.h"

namespace service::provider {

class CreateCertificateProviderValidator final
    : public ruvia::Middleware<CreateCertificateProviderValidator> {
    RUVIA_VALIDATE_JSON(
        CreateCertificateProviderBody,
        RUVIA_RULE(provider, RUVIA_REQUIRED("请选择供应商类型"),
                   RUVIA_REGEX("供应商类型不正确", R"(^(letsencrypt|zerossl)$)")),
        RUVIA_RULE_NAME("credential_mode", credentialMode, RUVIA_REQUIRED("请选择接入方式"),
                        RUVIA_REGEX("接入方式不正确", R"(^(email|access_key)$)")),
        RUVIA_RULE_NAME(
            "account_email", accountEmail, RUVIA_MAX(254, "账户邮箱最多254个字符"),
            RUVIA_REGEX(
                "账户邮箱格式不正确",
                R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$)")),
        RUVIA_RULE_NAME("access_key", accessKey, RUVIA_MIN(1, "API Access Key 不能为空"),
                        RUVIA_MAX(255, "API Access Key 最多255个字符"),
                        RUVIA_REGEX("API Access Key 格式不正确", R"(^\S+$)")))
};

class UpdateCertificateProviderValidator final
    : public ruvia::Middleware<UpdateCertificateProviderValidator> {
    RUVIA_VALIDATE_JSON(
        UpdateCertificateProviderBody,
        RUVIA_RULE_NAME("credential_mode", credentialMode, RUVIA_REQUIRED("请选择接入方式"),
                        RUVIA_REGEX("接入方式不正确", R"(^(email|access_key)$)")),
        RUVIA_RULE_NAME(
            "account_email", accountEmail, RUVIA_MAX(254, "账户邮箱最多254个字符"),
            RUVIA_REGEX(
                "账户邮箱格式不正确",
                R"(^[A-Za-z0-9.!#$%&'*+/=?^_`{|}~-]+@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$)")),
        RUVIA_RULE_NAME("access_key", accessKey, RUVIA_MIN(1, "API Access Key 不能为空"),
                        RUVIA_MAX(255, "API Access Key 最多255个字符"),
                        RUVIA_REGEX("API Access Key 格式不正确", R"(^\S+$)")))
};

} // namespace service::provider
