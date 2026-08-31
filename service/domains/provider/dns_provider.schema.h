#pragma once

#include <ruvia/web/Controller.h>

#include "service/domains/provider/dns_provider.types.h"

namespace service::provider {

class CreateDnsProviderValidator final : public ruvia::Middleware<CreateDnsProviderValidator> {
    RUVIA_VALIDATE_JSON(
        CreateDnsProviderBody,
        RUVIA_RULE(name, RUVIA_REQUIRED("账号名称不能为空"), RUVIA_MIN(1, "账号名称不能为空"),
                   RUVIA_MAX(100, "账号名称最多100个字符"),
                   RUVIA_REGEX("账号名称不能为空", R"(^.*\S.*$)")),
        RUVIA_RULE(provider, RUVIA_REQUIRED("服务商类型不能为空"),
                   RUVIA_REGEX("服务商类型不支持", R"(^(cloudflare|aliyun)$)")),
        RUVIA_RULE_NAME("account_id", accountId, RUVIA_REQUIRED("账户标识不能为空"),
                        RUVIA_MIN(8, "账户标识格式不正确"), RUVIA_MAX(128, "账户标识格式不正确"),
                        RUVIA_REGEX("账户标识格式不正确", R"(^\S+$)")),
        RUVIA_RULE_NAME("api_token", apiToken, RUVIA_REQUIRED("访问密钥不能为空"),
                        RUVIA_MIN(16, "访问密钥格式不正确"), RUVIA_MAX(256, "访问密钥格式不正确")))
};

class UpdateDnsProviderValidator final : public ruvia::Middleware<UpdateDnsProviderValidator> {
    RUVIA_VALIDATE_JSON(UpdateDnsProviderBody,
                        RUVIA_RULE(name, RUVIA_REQUIRED("账号名称不能为空"),
                                   RUVIA_MIN(1, "账号名称不能为空"),
                                   RUVIA_MAX(100, "账号名称最多100个字符"),
                                   RUVIA_REGEX("账号名称不能为空", R"(^.*\S.*$)")),
                        RUVIA_RULE_NAME("api_token", apiToken, RUVIA_MIN(16, "访问密钥格式不正确"),
                                        RUVIA_MAX(256, "访问密钥格式不正确")))
};

} // namespace service::provider
