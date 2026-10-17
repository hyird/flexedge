#pragma once

#include <ruvia/web/model_json.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/route_policy.h"
#include "service/features/website_config/model.h"
#include "service/features/website_config/transport.h"

namespace service::website_config {

[[nodiscard]] inline std::optional<website_domain_data> normalize(const website_domain_input& input) {
    const auto& id = input.get<"id">();
    const auto& hostname = input.get<"hostname">();
    const auto& dns_mode = input.get<"dns_mode">();
    if (!id || !hostname || !dns_mode) {
        return std::nullopt;
    }
    return website_domain_data{.id_ = std::string(id->view()),
        .hostname_ = std::string(hostname->view()),
        .dns_mode_ = std::string(dns_mode->view())};
}

[[nodiscard]] inline std::optional<website_origin_data> normalize(const website_origin_input& input) {
    const auto& id = input.get<"id">();
    const auto& group = input.get<"group">();
    const auto& protocol = input.get<"protocol">();
    const auto& host = input.get<"host">();
    const auto& port = input.get<"port">();
    const auto& role = input.get<"role">();
    const auto& weight = input.get<"weight">();
    const auto& status = input.get<"status">();
    if (!id || !group || !protocol || !host || !port || !role || !weight || !status) {
        return std::nullopt;
    }
    return website_origin_data{.id_ = std::string(id->view()),
        .group_ = std::string(group->view()),
        .protocol_ = std::string(protocol->view()),
        .host_ = std::string(host->view()),
        .port_ = port->value_,
        .role_ = std::string(role->view()),
        .weight_ = weight->value_,
        .status_ = std::string(status->view())};
}

[[nodiscard]] inline std::optional<website_route_header_data>
normalize(const website_route_header_input& input) {
    const auto& name = input.get<"name">();
    const auto& value = input.get<"value">();
    if (!name || !value) {
        return std::nullopt;
    }
    return website_route_header_data{.name_ = std::string(name->view()),
        .value_ = std::string(value->view())};
}

[[nodiscard]] inline std::optional<website_route_rule_data>
normalize(const website_route_rule_input& input) {
    const auto& id = input.get<"id">();
    const auto& status = input.get<"status">();
    const auto& match_type = input.get<"match_type">();
    const auto& path = input.get<"path">();
    const auto& methods = input.get<"methods">();
    const auto& action = input.get<"action">();
    const auto& rewrite_path = input.get<"rewrite_path">();
    const auto& redirect_url = input.get<"redirect_url">();
    const auto& redirect_status = input.get<"redirect_status">();
    const auto& origin_group = input.get<"origin_group">();
    const auto& request_headers = input.get<"request_headers">();
    const auto& response_headers = input.get<"response_headers">();
    if (!id || !status || !match_type || !path || !methods || !action || !rewrite_path ||
        !redirect_url || !redirect_status || !request_headers || !response_headers) {
        return std::nullopt;
    }
    website_route_rule_data result{.id_ = std::string(id->view()),
        .name_ = input.get<"name">() ? std::string(input.get<"name">()->view()) : "",
        .description_ = input.get<"description">() ? std::string(input.get<"description">()->view()) : "",
        .hostnames_ = {},
        .conditions_ = {},
        .rewrite_mode_ = input.get<"rewrite_mode">() ? std::string(input.get<"rewrite_mode">()->view()) : (rewrite_path->empty() ? "none" : "replace_path"),
        .query_mode_ = input.get<"query_mode">() ? std::string(input.get<"query_mode">()->view()) : "preserve",
        .query_string_ = input.get<"query_string">() ? std::string(input.get<"query_string">()->view()) : "",
        .status_ = std::string(status->view()),
        .match_type_ = std::string(match_type->view()),
        .path_ = std::string(path->view()),
        .methods_ = {},
        .action_ = std::string(action->view()),
        .rewrite_path_ = std::string(rewrite_path->view()),
        .redirect_url_ = std::string(redirect_url->view()),
        .redirect_status_ = redirect_status->value_,
        .origin_group_ = origin_group ? std::string(origin_group->view()) : "",
        .request_headers_ = {},
        .response_headers_ = {}};
    if (const auto& hostnames = input.get<"hostnames">()) {
        for (const auto& host : *hostnames) {
            result.hostnames_.push_back(flexedge::route_policy::hostname(host.view()));
        }
    }
    result.methods_.reserve(methods->size());
    if (const auto& conditions = input.get<"conditions">()) {
        for (const auto& condition : *conditions) {
            const auto& source = condition.get<"source">();
            const auto& name = condition.get<"name">();
            const auto& op = condition.get<"op">();
            const auto& value = condition.get<"value">();
            if (!source || !name || !op || !value) {
                return std::nullopt;
            }
            result.conditions_.push_back({std::string(source->view()), std::string(name->view()),
                std::string(op->view()), std::string(value->view())});
        }
    }
    for (const auto& method : *methods) {
        result.methods_.emplace_back(method.view());
    }
    result.request_headers_.reserve(request_headers->size());
    for (const auto& header : *request_headers) {
        auto normalized = normalize(header);
        if (!normalized) {
            return std::nullopt;
        }
        result.request_headers_.push_back(std::move(*normalized));
    }
    result.response_headers_.reserve(response_headers->size());
    for (const auto& header : *response_headers) {
        auto normalized = normalize(header);
        if (!normalized) {
            return std::nullopt;
        }
        result.response_headers_.push_back(std::move(*normalized));
    }
    return result;
}

namespace detail {

template <typename values_type>
[[nodiscard]] inline std::vector<std::string> copy_strings(const values_type& values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.emplace_back(value.view());
    }
    return result;
}

}  // namespace detail

enum class route_origin_reference_status {
    valid,
    redirect_forbids_origin_group,
    missing_enabled_origin_group,
};

[[nodiscard]] inline std::unordered_set<std::string_view>
collect_enabled_origin_groups(const std::vector<website_origin_data>& origins) {
    std::unordered_set<std::string_view> groups;
    for (const auto& origin : origins) {
        if (origin.status_ == "enabled") {
            groups.emplace(origin.group_);
        }
    }
    return groups;
}

[[nodiscard]] inline std::unordered_set<std::string_view>
collect_enabled_origin_groups(const ruvia::array<website_origin_input>& origins) {
    std::unordered_set<std::string_view> groups;
    for (const auto& origin : origins) {
        const auto& group = origin.get<"group">();
        const auto& status = origin.get<"status">();
        if (group && status && status->view() == "enabled") {
            groups.emplace(group->view());
        }
    }
    return groups;
}

[[nodiscard]] inline route_origin_reference_status check_route_origin_reference(
    std::string_view action, std::string_view origin_group,
    const std::unordered_set<std::string_view>& enabled_origin_groups) {
    if (action == "redirect" && !origin_group.empty()) {
        return route_origin_reference_status::redirect_forbids_origin_group;
    }
    if (action == "proxy" && !enabled_origin_groups.contains(origin_group)) {
        return route_origin_reference_status::missing_enabled_origin_group;
    }
    return route_origin_reference_status::valid;
}

[[nodiscard]] inline std::optional<website_config_data> normalize(const website_config_input& input) {
    const auto& domains = input.get<"domains">();
    const auto& origins = input.get<"origins">();
    const auto& default_origin_group = input.get<"default_origin_group">();
    const auto& origin_host_header = input.get<"origin_host_header">();
    const auto& origin_connect_timeout_seconds = input.get<"origin_connect_timeout_seconds">();
    const auto& origin_read_timeout_seconds = input.get<"origin_read_timeout_seconds">();
    const auto& pass_client_ip = input.get<"pass_client_ip">();
    const auto& health_check_enabled = input.get<"health_check_enabled">();
    const auto& health_check_path = input.get<"health_check_path">();
    const auto& health_check_interval_seconds = input.get<"health_check_interval_seconds">();
    const auto& health_check_timeout_seconds = input.get<"health_check_timeout_seconds">();
    const auto& health_check_expected_status = input.get<"health_check_expected_status">();
    const auto& healthy_threshold = input.get<"healthy_threshold">();
    const auto& unhealthy_threshold = input.get<"unhealthy_threshold">();
    const auto& access_log_enabled = input.get<"access_log_enabled">();
    const auto& access_log_request_headers = input.get<"access_log_request_headers">();
    const auto& access_log_request_body = input.get<"access_log_request_body">();
    const auto& access_log_response_headers = input.get<"access_log_response_headers">();
    const auto& access_log_query_params = input.get<"access_log_query_params">();
    const auto& access_log_cookies = input.get<"access_log_cookies">();
    const auto& access_log_referer = input.get<"access_log_referer">();
    const auto& access_log_user_agent = input.get<"access_log_user_agent">();
    const auto& access_log_status_code_ranges = input.get<"access_log_status_code_ranges">();
    const auto& access_log_client_abort = input.get<"access_log_client_abort">();
    const auto& https_enabled = input.get<"https_enabled">();
    const auto& certificate_ids = input.get<"certificate_ids">();
    const auto& minimum_tls_version = input.get<"minimum_tls_version">();
    const auto& force_https = input.get<"force_https">();
    const auto& hsts_enabled = input.get<"hsts_enabled">();
    const auto& response_compression_enabled = input.get<"response_compression_enabled">();
    const auto& response_compression_min_bytes = input.get<"response_compression_min_bytes">();
    const auto& response_compression_max_bytes = input.get<"response_compression_max_bytes">();
    const auto& response_compression_algorithms = input.get<"response_compression_algorithms">();
    const auto& response_compression_mime_types = input.get<"response_compression_mime_types">();
    const auto& response_compression_extensions = input.get<"response_compression_extensions">();
    const auto& response_compression_excluded_extensions =
        input.get<"response_compression_excluded_extensions">();
    const auto& route_rules = input.get<"route_rules">();
    if (!domains || !origins || !default_origin_group || !origin_host_header ||
        !origin_connect_timeout_seconds || !origin_read_timeout_seconds || !pass_client_ip ||
        !health_check_enabled || !health_check_path || !health_check_interval_seconds ||
        !health_check_timeout_seconds || !health_check_expected_status || !healthy_threshold ||
        !unhealthy_threshold || !access_log_enabled || !access_log_request_headers ||
        !access_log_request_body || !access_log_response_headers || !access_log_query_params ||
        !access_log_cookies || !access_log_referer || !access_log_user_agent ||
        !access_log_status_code_ranges || !access_log_client_abort || !https_enabled || !certificate_ids ||
        !minimum_tls_version || !force_https || !hsts_enabled ||
        !response_compression_enabled || !response_compression_min_bytes ||
        !response_compression_max_bytes || !response_compression_algorithms ||
        !response_compression_mime_types || !response_compression_extensions ||
        !response_compression_excluded_extensions || !route_rules) {
        return std::nullopt;
    }

    website_config_data result;
    result.cache_enabled_ = input.get<"cache_enabled">() && input.get<"cache_enabled">()->value_;
    result.cache_policy_id_ = input.get<"cache_policy_id">() ? std::string(input.get<"cache_policy_id">()->view()) : "";
    if (const auto& name = input.get<"name">()) {
        result.name_ = std::string(name->view());
    }
    result.origin_host_header_ = std::string(origin_host_header->view());
    const auto& version=input.get<"origin_http_version">();
    result.origin_http_version_=version?std::string(version->view()):"auto";
    result.default_origin_group_ = std::string(default_origin_group->view());
    result.origin_connect_timeout_seconds_ = origin_connect_timeout_seconds->value_;
    result.origin_read_timeout_seconds_ = origin_read_timeout_seconds->value_;
    result.pass_client_ip_ = pass_client_ip->value_;
    result.health_check_enabled_ = health_check_enabled->value_;
    result.health_check_path_ = std::string(health_check_path->view());
    result.health_check_interval_seconds_ = health_check_interval_seconds->value_;
    result.health_check_timeout_seconds_ = health_check_timeout_seconds->value_;
    result.health_check_expected_status_ = health_check_expected_status->value_;
    result.healthy_threshold_ = healthy_threshold->value_;
    result.unhealthy_threshold_ = unhealthy_threshold->value_;
    result.access_log_enabled_ = access_log_enabled->value_;
    result.access_log_request_headers_ = access_log_request_headers->value_;
    result.access_log_request_body_ = access_log_request_body->value_;
    result.access_log_response_headers_ = access_log_response_headers->value_;
    result.access_log_query_params_ = access_log_query_params->value_;
    result.access_log_cookies_ = access_log_cookies->value_;
    result.access_log_referer_ = access_log_referer->value_;
    result.access_log_user_agent_ = access_log_user_agent->value_;
    result.access_log_status_code_ranges_ = detail::copy_strings(*access_log_status_code_ranges);
    result.access_log_client_abort_ = access_log_client_abort->value_;
    result.https_enabled_ = https_enabled->value_;
    result.certificate_ids_ = detail::copy_strings(*certificate_ids);
    result.minimum_tls_version_ = std::string(minimum_tls_version->view());
    result.force_https_ = force_https->value_;
    result.hsts_enabled_ = hsts_enabled->value_;
    result.response_compression_enabled_ = response_compression_enabled->value_;
    result.response_compression_min_bytes_ = response_compression_min_bytes->value_;
    result.response_compression_max_bytes_ = response_compression_max_bytes->value_;
    result.response_compression_algorithms_ = detail::copy_strings(*response_compression_algorithms);
    result.response_compression_mime_types_ = detail::copy_strings(*response_compression_mime_types);
    result.response_compression_extensions_ = detail::copy_strings(*response_compression_extensions);
    result.response_compression_excluded_extensions_ =
        detail::copy_strings(*response_compression_excluded_extensions);

    result.domains_.reserve(domains->size());
    for (const auto& domain : *domains) {
        auto normalized = normalize(domain);
        if (!normalized) {
            return std::nullopt;
        }
        result.domains_.push_back(std::move(*normalized));
    }
    result.origins_.reserve(origins->size());
    for (const auto& origin : *origins) {
        auto normalized = normalize(origin);
        if (!normalized) {
            return std::nullopt;
        }
        result.origins_.push_back(std::move(*normalized));
    }
    result.route_rules_.reserve(route_rules->size());
    for (const auto& rule : *route_rules) {
        auto normalized = normalize(rule);
        if (!normalized) {
            return std::nullopt;
        }
        result.route_rules_.push_back(std::move(*normalized));
    }
    const auto enabled_origin_groups = collect_enabled_origin_groups(result.origins_);
    if (!enabled_origin_groups.contains(result.default_origin_group_)) {
        return std::nullopt;
    }
    for (const auto& rule : result.route_rules_) {
        if (check_route_origin_reference(rule.action_, rule.origin_group_, enabled_origin_groups) !=
            route_origin_reference_status::valid) {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] inline bool complete(const website_domain_input& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const website_origin_input& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const website_config_input& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline std::optional<website_config_data>
parse_stored(std::string_view json, ruvia::model_parse_options options = {}) {
    const std::optional<website_config_input> input =
        ruvia::from_json<website_config_input>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline website_config_output to_output(const website_config_data& input,
    ruvia::model_options options = {}) {
    website_config_output output(options);
    output.set<"cache_enabled">(input.cache_enabled_);
    output.set<"cache_policy_id">(input.cache_policy_id_);
    if (input.name_) {
        output.set<"name">(*input.name_);
    }
    output.set<"origin_host_header">(input.origin_host_header_);
    output.set<"origin_http_version">(input.origin_http_version_);
    output.set<"default_origin_group">(input.default_origin_group_);
    output.set<"origin_connect_timeout_seconds">(input.origin_connect_timeout_seconds_);
    output.set<"origin_read_timeout_seconds">(input.origin_read_timeout_seconds_);
    output.set<"pass_client_ip">(input.pass_client_ip_);
    output.set<"health_check_enabled">(input.health_check_enabled_);
    output.set<"health_check_path">(input.health_check_path_);
    output.set<"health_check_interval_seconds">(input.health_check_interval_seconds_);
    output.set<"health_check_timeout_seconds">(input.health_check_timeout_seconds_);
    output.set<"health_check_expected_status">(input.health_check_expected_status_);
    output.set<"healthy_threshold">(input.healthy_threshold_);
    output.set<"unhealthy_threshold">(input.unhealthy_threshold_);
    output.set<"access_log_enabled">(input.access_log_enabled_);
    output.set<"access_log_request_headers">(input.access_log_request_headers_);
    output.set<"access_log_request_body">(input.access_log_request_body_);
    output.set<"access_log_response_headers">(input.access_log_response_headers_);
    output.set<"access_log_query_params">(input.access_log_query_params_);
    output.set<"access_log_cookies">(input.access_log_cookies_);
    output.set<"access_log_referer">(input.access_log_referer_);
    output.set<"access_log_user_agent">(input.access_log_user_agent_);
    output.set<"access_log_client_abort">(input.access_log_client_abort_);
    output.set<"https_enabled">(input.https_enabled_);
    output.set<"minimum_tls_version">(input.minimum_tls_version_);
    output.set<"force_https">(input.force_https_);
    output.set<"hsts_enabled">(input.hsts_enabled_);
    output.set<"response_compression_enabled">(input.response_compression_enabled_);
    output.set<"response_compression_min_bytes">(input.response_compression_min_bytes_);
    output.set<"response_compression_max_bytes">(input.response_compression_max_bytes_);
    const auto copy_strings = [&](auto& target, const std::vector<std::string>& values) {
        target.reserve(values.size());
        for (const auto& value : values) {
            target.emplace_back(value, options);
        }
    };
    copy_strings(output.ensure<"response_compression_algorithms">(),
        input.response_compression_algorithms_);
    copy_strings(output.ensure<"response_compression_mime_types">(),
        input.response_compression_mime_types_);
    copy_strings(output.ensure<"response_compression_extensions">(),
        input.response_compression_extensions_);
    copy_strings(output.ensure<"response_compression_excluded_extensions">(),
        input.response_compression_excluded_extensions_);
    copy_strings(output.ensure<"access_log_status_code_ranges">(), input.access_log_status_code_ranges_);
    auto& domains = output.ensure<"domains">();
    domains.reserve(input.domains_.size());
    for (const auto& domain : input.domains_) {
        auto& item = domains.emplace_back(options);
        item.set<"id">(domain.id_);
        item.set<"hostname">(domain.hostname_);
        item.set<"dns_mode">(domain.dns_mode_);
    }
    auto& origins = output.ensure<"origins">();
    origins.reserve(input.origins_.size());
    for (const auto& origin : input.origins_) {
        auto& item = origins.emplace_back(options);
        item.set<"id">(origin.id_);
        item.set<"group">(origin.group_);
        item.set<"protocol">(origin.protocol_);
        item.set<"host">(origin.host_);
        item.set<"port">(origin.port_);
        item.set<"role">(origin.role_);
        item.set<"weight">(origin.weight_);
        item.set<"status">(origin.status_);
    }
    auto& certificates = output.ensure<"certificate_ids">();
    certificates.reserve(input.certificate_ids_.size());
    for (const auto& id : input.certificate_ids_) {
        certificates.emplace_back(id, options);
    }
    auto copy_headers = [&](auto& target, const std::vector<website_route_header_data>& headers) {
        target.reserve(headers.size());
        for (const auto& header : headers) {
            auto& item = target.emplace_back(options);
            item.template set<"name">(header.name_);
            item.template set<"value">(header.value_);
        }
    };
    auto& route_rules = output.ensure<"route_rules">();
    route_rules.reserve(input.route_rules_.size());
    for (const auto& rule : input.route_rules_) {
        auto& item = route_rules.emplace_back(options);
        item.set<"id">(rule.id_);
        item.set<"name">(rule.name_);
        item.set<"description">(rule.description_);
        copy_strings(item.ensure<"hostnames">(), rule.hostnames_);
        auto& conditions = item.ensure<"conditions">();
        for (const auto& condition : rule.conditions_) {
            auto& target = conditions.emplace_back(options);
            target.set<"source">(condition.source_);
            target.set<"name">(condition.name_);
            target.set<"op">(condition.op_);
            target.set<"value">(condition.value_);
        }
        item.set<"rewrite_mode">(rule.rewrite_mode_);
        item.set<"query_mode">(rule.query_mode_);
        item.set<"query_string">(rule.query_string_);
        item.set<"status">(rule.status_);
        item.set<"match_type">(rule.match_type_);
        item.set<"path">(rule.path_);
        item.set<"action">(rule.action_);
        item.set<"rewrite_path">(rule.rewrite_path_);
        item.set<"redirect_url">(rule.redirect_url_);
        item.set<"redirect_status">(rule.redirect_status_);
        copy_strings(item.ensure<"methods">(), rule.methods_);
        item.set<"origin_group">(rule.origin_group_);
        copy_headers(item.ensure<"request_headers">(), rule.request_headers_);
        copy_headers(item.ensure<"response_headers">(), rule.response_headers_);
    }
    return output;
}

}  // namespace service::website_config
