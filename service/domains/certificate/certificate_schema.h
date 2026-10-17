#pragma once

#include <ruvia/web/controller.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/domains/certificate/certificate_types.h"
#include "service/features/certificate/config_mapper.h"

namespace service::certificate {

inline void validate_create_certificate(
    const create_certificate_body& body, ruvia::validator& validator) {
    const auto& domain = body.get<"domain">();
    validator.required(domain, "domain", "域名不能为空");
    if (domain) {
        validator.min_length(domain, "domain", 1, "域名不能为空");
        validator.max_length(domain, "domain", 253, "域名最多253个字符");
        if (!service::common::is_hostname(domain->view())) {
            validator.add("domain", "regex", "域名格式不正确");
        }
    }

    const auto& provider_id = body.get<"certificate_provider_id">();
    validator.required(provider_id, "certificate_provider_id", "请选择证书供应商");
    if (provider_id && !service::common::parse_uuid(std::optional<std::string_view>{provider_id->view()})) {
        validator.add("certificate_provider_id", "regex", "证书供应商不正确");
    }

    const auto& dns_zone_id = body.get<"dns_zone_id">();
    validator.required(dns_zone_id, "dns_zone_id", "请选择托管域名");
    if (dns_zone_id && !service::common::parse_uuid(std::optional<std::string_view>{dns_zone_id->view()})) {
        validator.add("dns_zone_id", "regex", "托管域名不正确");
    }

    const auto& config = body.get<"config">();
    validator.required(config, "config", "证书配置不能为空");
    if (config && !service::certificate_issuance::normalize(*config).has_value()) {
        validator.add("config", "custom", "证书配置不正确");
    }
}

using create_certificate_validator =
    service::common::json_validator_middleware<create_certificate_body, validate_create_certificate>;
using certificate_config_validator = service::common::json_validator_middleware<
    service::certificate_issuance::certificate_config_input,
    service::certificate_issuance::validate_certificate_config>;

}  // namespace service::certificate
