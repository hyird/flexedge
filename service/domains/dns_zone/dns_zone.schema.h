#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

#include <ruvia/web/Controller.h>

#include "service/common/types.h"
#include "service/domains/dns_zone/dns_zone.types.h"

namespace service::dns_zone {

class CreateDnsZoneValidator final : public ruvia::Middleware<CreateDnsZoneValidator> {
    RUVIA_VALIDATE_JSON(
        CreateDnsZoneBody,
        RUVIA_RULE_NAME("dns_provider_id", dnsProviderId, RUVIA_REQUIRED("请选择 DNS 服务商账号"),
                        RUVIA_REGEX("DNS 服务商账号不正确", service::common::kUuidPattern)),
        RUVIA_RULE(
            domain, RUVIA_REQUIRED("域名不能为空"), RUVIA_MIN(1, "域名不能为空"),
            RUVIA_MAX(253, "域名最多253个字符"),
            RUVIA_REGEX("域名格式不正确",
                        R"(^([a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]{2,63}$)")))
};

struct DnsRecordConfigValidator final {
    template <typename ValidatorT>
    void validateNested(const service::dns_sync::ZoneRecordInput& value, std::string_view path,
                        ValidatorT& validator) const {
        const auto requireString = [&](std::string_view field, const auto& input,
                                       std::size_t maximum, std::string_view message) {
            const auto fieldPath = std::string(path) + "." + std::string(field);
            if (!input || input->empty()) {
                validator.add(fieldPath, "required", message);
                return false;
            }
            if (input->size() > maximum) {
                validator.add(fieldPath, "too_big", message);
                return false;
            }
            return true;
        };

        const auto& id = value.get<"id">();
        const bool validId = requireString("id", id, 36, "记录 ID 不正确");
        if (validId && id &&
            !service::common::parseUuid(std::optional<std::string_view>{id->view()})) {
            validator.add(std::string(path) + ".id", "format", "记录 ID 不正确");
        }

        const auto& type = value.get<"type">();
        const bool validType = requireString("type", type, 8, "记录类型不能为空");
        if (validType && type) {
            const auto typeView = type->view();
            if (typeView != "A" && typeView != "AAAA" && typeView != "CNAME" && typeView != "TXT" &&
                typeView != "MX") {
                validator.add(std::string(path) + ".type", "enum", "记录类型不支持");
            }
        }

        (void)requireString("name", value.get<"name">(), 253, "主机记录不能为空");
        (void)requireString("content", value.get<"content">(), 4096, "记录值不能为空");
        if (const auto& ttl = value.get<"ttl">(); !ttl || ttl->value < 1 || ttl->value > 86400) {
            validator.add(std::string(path) + ".ttl", "range", "TTL 不正确");
        }
        if (const auto& priority = value.get<"priority">();
            priority && (priority->value < 0 || priority->value > 65535)) {
            validator.add(std::string(path) + ".priority", "range", "优先级不正确");
        }
        if (!value.get<"proxied">()) {
            validator.add(std::string(path) + ".proxied", "required", "代理状态不能为空");
        }
        (void)requireString("line_code", value.get<"lineCode">(), 64, "请选择 DNS 线路");
    }
};

inline bool hasUniqueRecordIds(const ruvia::Array<service::dns_sync::ZoneRecordInput>& records) {
    std::unordered_set<std::string_view> ids;
    ids.reserve(records.size());
    for (const auto& record : records) {
        const auto& id = record.get<"id">();
        if (!id || !ids.insert(id->view()).second) {
            return false;
        }
    }
    return true;
}

class DnsZoneConfigValidator final : public ruvia::Middleware<DnsZoneConfigValidator> {
    RUVIA_VALIDATE_JSON(service::dns_sync::ZoneConfigInput,
                        RUVIA_RULE(records, RUVIA_MAX(10000, "单个域名最多保存10000条记录"),
                                   RUVIA_CUSTOM("记录 ID 不能重复", hasUniqueRecordIds),
                                   RUVIA_EACH(DnsRecordConfigValidator)))
};

inline void validateDnsZoneSync(const DnsZoneSyncBody& body, ruvia::Validator& validator) {
    validator.oneOf(body.get<"conflictPolicy">(), "conflict_policy", {"local", "remote"},
                    "冲突处理方式不正确");
}

} // namespace service::dns_zone
