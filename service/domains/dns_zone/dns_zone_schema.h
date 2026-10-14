#pragma once

#include <ruvia/http/http_ascii.h>
#include <ruvia/http/http_limits.h>
#include <ruvia/http/http_media_type.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/model_json.h>

#include <cstddef>
#include <optional>
#include <string_view>

#include "service/domains/dns_zone/dns_zone_types.h"

namespace service::dns_zone {

class create_dns_zone_validator final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        const auto& body = context.req().validated_json<create_dns_zone_body>().value();
        ruvia::validator validator({.resource_ = context.pool()});
        const auto& provider_id = body.get<"dns_provider_id">();
        validator.required(provider_id, "dns_provider_id", "请选择 DNS 服务商账号");
        if (provider_id && !is_valid_uuid(*provider_id)) {
            validator.add("dns_provider_id", "regex", "DNS 服务商账号不正确");
        }

        const auto& domain = body.get<"domain">();
        validator.required(domain, "domain", "域名不能为空");
        if (domain) {
            validator.min_length(domain, "domain", 1, "域名不能为空");
            validator.max_length(domain, "domain", 253, "域名最多253个字符");
            if (!is_valid_zone_domain(*domain)) {
                validator.add("domain", "regex", "域名格式不正确");
            }
        }
        validator.throw_if_invalid();
        co_await next();
    }
};

// Ruvia's json_body parses with its default budget (64Ki array elements, 16 MiB of charged
// representation). Each record element is charged 4 * (sizeof(zone_record_input) + pointer), so
// valid configurations well below the 10000-record domain limit were rejected as "invalid json
// body". The previous Ruvia release had no parse budget; only the request body limit applied.
// This budget restores that: no element takes less than one input byte and no charge exceeds a
// full record element per input byte (keys and strings cost at most 32 bytes per input byte), so
// every document within the server's buffered body limit fits.
inline constexpr std::size_t zone_config_max_body_bytes = ruvia::default_max_buffered_body_bytes;
inline constexpr std::size_t zone_config_max_array_elements = zone_config_max_body_bytes;
inline constexpr std::size_t zone_config_max_representation_bytes =
    sizeof(service::dns_sync::zone_config_input) +
    zone_config_max_body_bytes * 4 *
        (sizeof(service::dns_sync::zone_record_input) + sizeof(service::dns_sync::zone_record_input*));

// Binds the DNS configuration body in place of json_body<zone_config_input> and runs the domain
// rules. Handlers read the model with context.request_state<service::dns_sync::zone_config_input>().
// A body that is not application/json, is malformed, or is structurally invalid is handed to
// json_body, which produces the same 415/400 responses and structural issues as before.
class dns_zone_config_body final {
public:
    ruvia::task<void> handle(ruvia::context& context, ruvia::next& next) {
        std::optional<service::dns_sync::zone_config_input> body;
        const auto content_type = context.req().header("Content-Type").value_or(std::string_view{});
        if (!content_type.empty() &&
            ruvia::http_ascii_equals_ignore_case(ruvia::http_media_type_only(content_type),
                "application/json")) {
            const auto text = co_await context.req().text();
            body = ruvia::from_json<service::dns_sync::zone_config_input>(
                text, {.resource_ = context.arena(),
                          .max_array_elements_ = zone_config_max_array_elements,
                          .max_representation_bytes_ = zone_config_max_representation_bytes});
        }
        if (!body) {
            ruvia::json_body<service::dns_sync::zone_config_input> standard;
            co_await standard.handle(context, next);
            co_return;
        }
        ruvia::validator validator({.resource_ = context.pool()});
        service::dns_sync::validate_zone_config(*body, validator);
        validator.throw_if_invalid();
        const auto binding = context.bind_request_state(*body);
        co_await next();
    }
};

inline void validate_dns_zone_sync(const dns_zone_sync_body& body, ruvia::validator& validator) {
    const auto& policy = body.get<"conflict_policy">();
    if (policy && policy->view() != "local" && policy->view() != "remote") {
        validator.add("conflict_policy", "one_of", "冲突处理方式不正确");
    }
}

}  // namespace service::dns_zone
