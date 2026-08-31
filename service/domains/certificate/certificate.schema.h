#pragma once

#include <ruvia/web/Controller.h>

#include "service/common/types.h"
#include "service/domains/certificate/certificate.types.h"

namespace service::certificate {

inline bool
validCertificateConfig(const service::certificate_issuance::CertificateConfigInput& config) {
    return service::certificate_issuance::normalize(config).has_value();
}

class CreateCertificateValidator final : public ruvia::Middleware<CreateCertificateValidator> {
    RUVIA_VALIDATE_JSON(
        CreateCertificateBody,
        RUVIA_RULE(
            domain, RUVIA_REQUIRED("域名不能为空"), RUVIA_MIN(1, "域名不能为空"),
            RUVIA_MAX(253, "域名最多253个字符"),
            RUVIA_REGEX(
                "域名格式不正确",
                R"(^(?:\*\.)?([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$)")),
        RUVIA_RULE_NAME("certificate_provider_id", certificateProviderId,
                        RUVIA_REQUIRED("请选择证书供应商"),
                        RUVIA_REGEX("证书供应商不正确", service::common::kUuidPattern)),
        RUVIA_RULE_NAME("dns_zone_id", dnsZoneId, RUVIA_REQUIRED("请选择托管域名"),
                        RUVIA_REGEX("托管域名不正确", service::common::kUuidPattern)),
        RUVIA_RULE(config, RUVIA_REQUIRED("证书配置不能为空"),
                   RUVIA_CUSTOM("证书配置不正确", validCertificateConfig)))
};

class CertificateConfigValidator final : public ruvia::Middleware<CertificateConfigValidator> {
    RUVIA_VALIDATE_JSON(service::certificate_issuance::CertificateConfigInput,
                        RUVIA_RULE_NAME("auto_renew", autoRenew,
                                        RUVIA_REQUIRED("自动续签设置不能为空")))
};

} // namespace service::certificate
