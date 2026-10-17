#pragma once

#include <ruvia/web/validation.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "common/route_match.h"
#include "node/proto/website_validation.h"
#include "service/common/domain_name.h"
#include "service/common/ip_address.h"
#include "service/common/uuid.h"
#include "service/features/website_config/mapper.h"
#include "service/features/website_config/transport.h"

namespace service::website_config {

inline constexpr std::size_t max_website_certificates{20};

inline bool is_origin_host(std::string_view value) {
    return service::common::is_ipv4_address(value) || service::common::is_hostname(value, false);
}

inline bool is_origin_host_header(std::string_view value) {
    return value == "$host" || service::common::is_hostname(value, false);
}

inline bool is_website_name(std::string_view value) {
    return !value.empty();
}

inline bool is_origin_group(std::string_view value) {
    return !value.empty() && value.size() <= 100 &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::iscntrl(ch) != 0; });
}

inline bool valid_compression_algorithms(const ruvia::array<ruvia::string>& values) {
    if (values.empty()) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if (!flexedge::node::compression_algorithm(value.view()) ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool valid_compression_values(const ruvia::array<ruvia::string>& values,
    flexedge::node::compression_match_kind kind) {
    if (values.size() > flexedge::node::max_response_compression_match_values) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if (!flexedge::node::compression_match_value(value.view(), kind) ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool valid_access_log_status_code_ranges(const ruvia::array<ruvia::string>& values) {
    if (values.empty()) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if (!flexedge::node::access_log_status_code_range(value.view()) ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool valid_route_methods(const ruvia::array<ruvia::string>& values) {
    if (values.size() > flexedge::node::max_route_methods) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if (!flexedge::node::route_method(value.view()) || !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

// The count limit is reported per list by the rule validator.
inline bool valid_route_header_set(const ruvia::array<website_route_header_input>& headers) {
    std::unordered_set<std::string> names;
    for (const auto& header : headers) {
        const auto& name = header.get<"name">();
        const auto& value = header.get<"value">();
        if (!name || !value || !flexedge::node::route_header_name(name->view()) ||
            !flexedge::node::route_header_value(value->view())) {
            return false;
        }
        std::string normalized(name->view());
        std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!names.emplace(std::move(normalized)).second) {
            return false;
        }
    }
    return true;
}

struct website_domain_validator final {
    template <typename validator_t_type>
    void validate_nested(const website_domain_input& value, std::string_view path,
        validator_t_type& validator) const {
        const auto& id = value.get<"id">();
        if (!id || !service::common::parse_uuid(id ? std::optional<std::string_view>{id->view()}
                                                   : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "域名 ID 不正确");
        }
        const auto& hostname = value.get<"hostname">();
        if (!hostname || hostname->empty()) {
            validator.add(std::string(path) + ".hostname", "required", "域名不能为空");
        } else if (!service::common::is_hostname(hostname->view())) {
            validator.add(std::string(path) + ".hostname", "format", "域名格式不正确");
        }
        const auto& mode = value.get<"dns_mode">();
        if (!mode || (mode->view() != "managed" && mode->view() != "external")) {
            validator.add(std::string(path) + ".dns_mode", "enum", "解析方式不正确");
        }
    }
};

struct website_origin_validator final {
    template <typename validator_t_type>
    void validate_nested(const website_origin_input& value, std::string_view path,
        validator_t_type& validator) const {
        const auto& id = value.get<"id">();
        if (!id || !service::common::parse_uuid(id ? std::optional<std::string_view>{id->view()}
                                                   : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "源站 ID 不正确");
        }
        const auto& group = value.get<"group">();
        if (!group || !is_origin_group(group->view())) {
            validator.add(std::string(path) + ".group", "format", "源站组名称不正确");
        }
        const auto& protocol = value.get<"protocol">();
        if (!protocol || (protocol->view() != "http" && protocol->view() != "https")) {
            validator.add(std::string(path) + ".protocol", "enum", "源站协议不正确");
        }
        const auto& host = value.get<"host">();
        if (!host || host->empty()) {
            validator.add(std::string(path) + ".host", "required", "源站地址不能为空");
        } else if (host->size() > 253 || !is_origin_host(host->view())) {
            validator.add(std::string(path) + ".host", "format", "源站地址格式不正确");
        }
        if (const auto& port = value.get<"port">();
            !port || port->value_ < 1 || port->value_ > 65535) {
            validator.add(std::string(path) + ".port", "range", "源站端口不正确");
        }
        const auto& role = value.get<"role">();
        if (!role || (role->view() != "primary" && role->view() != "backup")) {
            validator.add(std::string(path) + ".role", "enum", "源站角色不正确");
        }
        if (const auto& weight = value.get<"weight">();
            !weight || weight->value_ < 1 || weight->value_ > 100) {
            validator.add(std::string(path) + ".weight", "range", "源站权重不正确");
        }
        const auto& status = value.get<"status">();
        if (!status || (status->view() != "enabled" && status->view() != "disabled")) {
            validator.add(std::string(path) + ".status", "enum", "源站状态不正确");
        }
    }
};

struct website_route_rule_validator final {
    template <typename validator_t_type>
    void validate_nested(const website_route_rule_input& value, std::string_view path,
        validator_t_type& validator) const {
        validate_metadata_and_hostnames(value, path, validator);
        const auto pattern = validate_match_and_headers(value, path, validator);
        validate_action_and_rewrite(value, path, pattern, validator);
    }

private:
    template <typename validator_t_type>
    static void validate_metadata_and_hostnames(const website_route_rule_input& value,
        std::string_view path, validator_t_type& validator) {
        namespace policy = flexedge::route_policy;
        namespace protocol = flexedge::node;
        if (const auto& name = value.get<"name">();
            name && !policy::clean_text(name->view(), protocol::max_route_name_bytes)) {
            validator.add(std::string(path) + ".name", "format",
                "规则名称最多" + std::to_string(protocol::max_route_name_bytes) +
                    "字节且不能包含控制字符");
        }
        if (const auto& description = value.get<"description">();
            description && description->view().size() > protocol::max_route_description_bytes) {
            validator.add(std::string(path) + ".description", "format",
                "规则备注最多" + std::to_string(protocol::max_route_description_bytes) + "字节");
        }
        if (const auto& hostnames = value.get<"hostnames">()) {
            std::unordered_set<std::string> seen;
            if (hostnames->size() > protocol::max_route_hostnames) {
                validator.add(std::string(path) + ".hostnames", "format",
                    "最多" + std::to_string(protocol::max_route_hostnames) + "个匹配域名");
            }
            for (const auto& host : *hostnames) {
                if (!policy::exact_hostname(host.view()) ||
                    !seen.emplace(policy::hostname(host.view())).second) {
                    validator.add(std::string(path) + ".hostnames", "format",
                        "请填写不重复的精确域名，不含协议、端口或通配符");
                    break;
                }
            }
        }
        const auto& id = value.get<"id">();
        if (!id || !service::common::parse_uuid(id ? std::optional<std::string_view>{id->view()}
                                                   : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "规则 ID 不正确");
        }
        const auto& status = value.get<"status">();
        if (!status || (status->view() != "enabled" && status->view() != "disabled")) {
            validator.add(std::string(path) + ".status", "enum", "规则状态不正确");
        }
    }

    template <typename validator_t_type>
    static std::shared_ptr<const RE2> validate_match_and_headers(
        const website_route_rule_input& value, std::string_view path, validator_t_type& validator) {
        namespace policy = flexedge::route_policy;
        namespace protocol = flexedge::node;
        const auto& match_type = value.get<"match_type">();
        if (!match_type || !policy::valid_match_type(match_type->view())) {
            validator.add(std::string(path) + ".match_type", "enum", "匹配方式不正确");
        }
        const auto& route_path = value.get<"path">();
        std::shared_ptr<const RE2> pattern;
        if (match_type && match_type->view() == "regex") {
            if (route_path) {
                pattern = policy::compile_pattern(route_path->view());
            }
            if (!pattern) {
                validator.add(
                    std::string(path) + ".path", "format",
                    "正则需符合 RE2 语法，最多512字节、9个捕获组，不支持回溯引用、前后查找或 \\C");
            }
        } else if (match_type && match_type->view() == "suffix") {
            if (!route_path || !protocol::route_suffix(route_path->view())) {
                validator.add(std::string(path) + ".path", "format",
                    "请填写路径后缀，不含空白、查询参数或片段");
            }
        } else if (!route_path || !protocol::route_path(route_path->view())) {
            validator.add(std::string(path) + ".path", "format", "匹配路径必须以 / 开头");
        }
        if (const auto& conditions = value.get<"conditions">()) {
            if (conditions->size() > protocol::max_route_conditions) {
                validator.add(std::string(path) + ".conditions", "format",
                    "最多" + std::to_string(protocol::max_route_conditions) + "个匹配条件");
            }
            std::size_t index = 0;
            for (const auto& condition : *conditions) {
                const auto& source = condition.get<"source">();
                const auto& name = condition.get<"name">();
                const auto& op = condition.get<"op">();
                const auto& text = condition.get<"value">();
                if (!source || !name || !op || !text ||
                    !policy::condition_options(source->view(), name->view(), op->view(),
                        text->view())) {
                    validator.add(std::string(path) + ".conditions." + std::to_string(index) +
                                      ".name",
                        "format", "条件来源、名称、操作符或值不正确");
                }
                ++index;
            }
        }
        const auto& methods = value.get<"methods">();
        if (!methods || !valid_route_methods(*methods)) {
            validator.add(std::string(path) + ".methods", "format", "请求方法不正确或重复");
        }
        const auto header_limit = std::to_string(protocol::max_route_headers);
        if (const auto& headers = value.get<"request_headers">();
            headers && headers->size() > protocol::max_route_headers) {
            validator.add(std::string(path) + ".request_headers", "too_big",
                "最多" + header_limit + "个请求头");
        }
        if (const auto& headers = value.get<"response_headers">();
            headers && headers->size() > protocol::max_route_headers) {
            validator.add(std::string(path) + ".response_headers", "too_big",
                "最多" + header_limit + "个响应头");
        }
        return pattern;
    }

    template <typename validator_t_type>
    static void validate_action_and_rewrite(const website_route_rule_input& value,
        std::string_view path, const std::shared_ptr<const RE2>& pattern,
        validator_t_type& validator) {
        namespace policy = flexedge::route_policy;
        const auto& action = value.get<"action">();
        if (!action || (action->view() != "proxy" && action->view() != "redirect" &&
                           action->view() != "rewrite")) {
            validator.add(std::string(path) + ".action", "enum", "规则动作不正确");
            return;
        }
        const auto& rewrite_path = value.get<"rewrite_path">();
        const auto& redirect_url = value.get<"redirect_url">();
        const auto& redirect_status = value.get<"redirect_status">();
        const auto& origin_group = value.get<"origin_group">();
        if (!rewrite_path || !redirect_url || !redirect_status || !origin_group) {
            return;
        }
        if (pattern &&
            (!policy::capture_template(rewrite_path->view(), pattern->NumberOfCapturingGroups()) ||
                !policy::capture_template(redirect_url->view(), pattern->NumberOfCapturingGroups()))) {
            validator.add(std::string(path) +
                              (action->view() == "proxy" ? ".rewrite_path" : ".redirect_url"),
                "format", "捕获替换仅支持正则中存在的 ${0} 到 ${9}，字面美元符用 $$");
        }
        const auto& configured_mode = value.get<"rewrite_mode">();
        const auto mode = configured_mode ? configured_mode->view() : std::string_view{};
        const auto& configured_query = value.get<"query_mode">();
        const auto query_mode = configured_query ? configured_query->view() : std::string_view{};
        const auto& query = value.get<"query_string">();
        const auto query_text = query ? query->view() : std::string_view{};
        if (!policy::rewrite_mode(mode)) {
            validator.add(std::string(path) + ".rewrite_mode", "enum", "重写方式不正确");
        }
        if (!policy::query_mode(query_mode)) {
            validator.add(std::string(path) + ".query_mode", "enum", "查询参数策略不正确");
        }
        if (!policy::query_string(query_text) || (query_mode != "replace" && !query_text.empty())) {
            validator.add(std::string(path) + ".query_string", "format",
                "仅替换策略可填写参数，不含开头问号、空白或片段标记");
        }
        if ((mode == "none" || mode == "strip_prefix") && !rewrite_path->empty()) {
            validator.add(std::string(path) + ".rewrite_path", "format",
                "当前重写方式无需填写替换路径");
        }
        if ((mode == "replace_path" || mode == "replace_prefix") && rewrite_path->empty()) {
            validator.add(std::string(path) + ".rewrite_path", "required", "请填写替换路径");
        }
        const auto& match_type = value.get<"match_type">();
        const auto& route_path = value.get<"path">();
        if ((mode == "strip_prefix" || mode == "replace_prefix") &&
            (!match_type || match_type->view() != "prefix" || !route_path ||
                !policy::path_only(route_path->view()) ||
                (mode == "replace_prefix" && !policy::path_only(rewrite_path->view())))) {
            validator.add(std::string(path) + ".rewrite_mode", "format",
                "前缀重写仅适用于前缀匹配，路径不能含查询参数或片段");
        }
        if (action->view() == "redirect" && !mode.empty() && mode != "none") {
            validator.add(std::string(path) + ".rewrite_mode", "format",
                "跳转规则不能同时重写回源路径");
        }
        if (action->view() == "rewrite") {
            const auto& request_headers = value.get<"request_headers">();
            const auto& response_headers = value.get<"response_headers">();
            if ((!rewrite_path->empty() && !policy::path_only(rewrite_path->view())) ||
                !redirect_url->empty() || redirect_status->value_ != 0 || !origin_group->empty() ||
                (request_headers && !request_headers->empty()) ||
                (response_headers && !response_headers->empty())) {
                validator.add(std::string(path), "format",
                    "重写规则不能配置源站、跳转或请求头，重写路径必须以 / 开头");
            }
        } else if (action->view() == "proxy") {
            if ((!rewrite_path->empty() && !policy::path_only(rewrite_path->view())) ||
                !redirect_url->empty() || redirect_status->value_ != 0 ||
                !is_origin_group(origin_group->view())) {
                validator.add(std::string(path), "format", "代理规则配置不正确");
            }
        } else if (!flexedge::node::route_redirect_url(redirect_url->view()) ||
                   !policy::redirect_status(redirect_status->value_) || !rewrite_path->empty() ||
                   !origin_group->empty()) {
            validator.add(std::string(path), "format", "重定向规则配置不正确");
        }
    }
};

inline bool valid_domain_set(const ruvia::array<website_domain_input>& domains) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> hostnames;
    for (const auto& domain : domains) {
        const auto& id = domain.get<"id">();
        const auto parsed_id = service::common::parse_uuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& hostname = domain.get<"hostname">();
        if (!parsed_id || !hostname || !ids.insert(*parsed_id).second) {
            return false;
        }
        std::string normalized(hostname->view());
        std::ranges::transform(normalized, normalized.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!hostnames.insert(std::move(normalized)).second) {
            return false;
        }
    }
    return true;
}

inline bool valid_origin_set(const ruvia::array<website_origin_input>& origins) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string_view> enabled_groups;
    std::unordered_set<std::string_view> enabled_primary_groups;
    for (const auto& origin : origins) {
        const auto& id = origin.get<"id">();
        const auto parsed_id = service::common::parse_uuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& group = origin.get<"group">();
        if (!parsed_id || !group || !ids.insert(*parsed_id).second) {
            return false;
        }
        const auto& role = origin.get<"role">();
        const auto& status = origin.get<"status">();
        if (status && status->view() == "enabled") {
            enabled_groups.emplace(group->view());
            if (role && role->view() == "primary") {
                enabled_primary_groups.emplace(group->view());
            }
        }
    }
    return !enabled_groups.empty() && enabled_groups == enabled_primary_groups;
}

inline bool valid_route_rule_set(const ruvia::array<website_route_rule_input>& rules) {
    std::unordered_set<std::string> ids;
    for (const auto& rule : rules) {
        const auto& id = rule.get<"id">();
        const auto parsed_id = service::common::parse_uuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& request_headers = rule.get<"request_headers">();
        const auto& response_headers = rule.get<"response_headers">();
        const auto& origin_group = rule.get<"origin_group">();
        if (!parsed_id || !request_headers || !response_headers || !ids.insert(*parsed_id).second ||
            !valid_route_header_set(*request_headers) || !valid_route_header_set(*response_headers)) {
            return false;
        }
        if (!origin_group) {
            return false;
        }
    }
    return true;
}

inline bool has_unique_certificate_ids(const ruvia::array<ruvia::string>& values) {
    std::unordered_set<std::string> ids;
    for (const auto& value : values) {
        const auto id = service::common::parse_uuid(value.view());
        if (!id || !ids.insert(*id).second) {
            return false;
        }
    }
    return true;
}

struct website_config_rules final {
    void validate_nested(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) const {
        const auto& name = body.get<"name">();
        validator.max_length(name, field_path(prefix, "name"), 100, "网站名称最多100个字符");
        add_if_invalid(validator, prefix, name, "name",
            [](const auto& v) { return is_website_name(v.view()); }, "custom", "网站名称不能为空");

        validate_domains(body, prefix, validator);
        validate_origins(body, prefix, validator);
        validate_health_check(body, prefix, validator);
        validate_access_log(body, prefix, validator);
        validate_https(body, prefix, validator);
        validate_compression(body, prefix, validator);
        validate_route_rules(body, prefix, validator);
        validate_origin_references(body, prefix, validator);
    }

private:
    [[nodiscard]] static std::string field_path(std::string_view prefix, std::string_view name) {
        std::string result(prefix);
        if (!result.empty()) {
            result.push_back('.');
        }
        result.append(name);
        return result;
    }

    template <typename value_type>
    static void require_field(ruvia::validator& validator, std::string_view prefix,
        const value_type& value, std::string_view name, std::string_view message) {
        validator.required(value, field_path(prefix, name), message);
    }

    template <typename value_type, typename limit_type>
    static void require_bounded_field(ruvia::validator& validator, std::string_view prefix,
        const value_type& value, std::string_view name, std::string_view required_message,
        limit_type minimum, limit_type maximum, std::string_view range_message) {
        const auto path = field_path(prefix, name);
        validator.required(value, path, required_message);
        validator.range(value, path, minimum, maximum, range_message);
    }

    template <typename value_type, typename predicate_type>
    static void add_if_invalid(ruvia::validator& validator, std::string_view prefix,
        const value_type& value, std::string_view name, predicate_type predicate,
        std::string_view code, std::string_view message) {
        if (value && !predicate(*value)) {
            validator.add(field_path(prefix, name), code, message);
        }
    }

    static void validate_domains(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        const auto& domains = body.get<"domains">();
        require_field(validator, prefix, domains, "domains", "至少需要一个绑定域名");
        if (!domains) {
            return;
        }
        if (domains->empty()) {
            validator.add(field_path(prefix, "domains"), "too_small", "至少需要一个绑定域名");
        }
        if (domains->size() > 100) {
            validator.add(field_path(prefix, "domains"), "too_big", "单个网站最多绑定100个域名");
        }
        add_if_invalid(validator, prefix, domains, "domains",
            [](const auto& v) { return valid_domain_set(v); }, "custom",
            "域名 ID 和主机名必须唯一");
        std::size_t index{};
        for (const auto& value : *domains) {
            website_domain_validator{}.validate_nested(
                value, field_path(prefix, "domains[" + std::to_string(index++) + "]"), validator);
        }
    }

    static void validate_origins(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        const auto& origins = body.get<"origins">();
        require_field(validator, prefix, origins, "origins", "至少需要一个源站");
        if (origins) {
            if (origins->empty()) {
                validator.add(field_path(prefix, "origins"), "too_small", "至少需要一个源站");
            }
            if (origins->size() > 100) {
                validator.add(field_path(prefix, "origins"), "too_big", "单个网站最多配置100个源站");
            }
            add_if_invalid(validator, prefix, origins, "origins",
                [](const auto& v) { return valid_origin_set(v); }, "custom",
                "源站 ID 必须唯一，且每个启用的源站组都需要主源站");
            std::size_t index{};
            for (const auto& value : *origins) {
                website_origin_validator{}.validate_nested(
                    value, field_path(prefix, "origins[" + std::to_string(index++) + "]"), validator);
            }
        }

        const auto& default_origin_group = body.get<"default_origin_group">();
        require_field(validator, prefix, default_origin_group, "default_origin_group",
            "请选择默认源站组");
        validator.max_length(default_origin_group, field_path(prefix, "default_origin_group"), 100,
            "默认源站组最多100个字符");
        add_if_invalid(validator, prefix, default_origin_group, "default_origin_group",
            [](const auto& v) { return is_origin_group(v.view()); }, "custom",
            "默认源站组名称不正确");
        const auto& origin_host_header = body.get<"origin_host_header">();
        const auto& origin_http_version = body.get<"origin_http_version">();
        if(origin_http_version && origin_http_version->view()!="auto" && origin_http_version->view()!="http1" && origin_http_version->view()!="http2") {
            validator.add(field_path(prefix,"origin_http_version"),"enum","回源 HTTP 版本不正确");
        }
        require_field(validator, prefix, origin_host_header, "origin_host_header",
            "回源 Host 不能为空");
        validator.max_length(origin_host_header, field_path(prefix, "origin_host_header"), 253,
            "回源 Host 最多253个字符");
        add_if_invalid(validator, prefix, origin_host_header, "origin_host_header",
            [](const auto& v) { return is_origin_host_header(v.view()); }, "custom",
            "请输入 $host 或有效域名");

        require_bounded_field(validator, prefix, body.get<"origin_connect_timeout_seconds">(),
            "origin_connect_timeout_seconds", "连接超时不能为空", 1, 300,
            "连接超时必须在1到300秒之间");
        require_bounded_field(validator, prefix, body.get<"origin_read_timeout_seconds">(),
            "origin_read_timeout_seconds", "读取超时不能为空", 1, 600,
            "读取超时必须在1到600秒之间");
        require_field(validator, prefix, body.get<"pass_client_ip">(), "pass_client_ip",
            "真实访客 IP 设置不能为空");
    }

    static void validate_health_check(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        require_field(validator, prefix, body.get<"health_check_enabled">(),
            "health_check_enabled", "健康检查设置不能为空");
        const auto& health_check_path = body.get<"health_check_path">();
        require_field(validator, prefix, health_check_path, "health_check_path",
            "健康检查路径不能为空");
        add_if_invalid(validator, prefix, health_check_path, "health_check_path",
            [](const auto& v) { return flexedge::node::route_path(v.view()); }, "custom",
            "健康检查路径必须以 / 开头");
        require_bounded_field(validator, prefix, body.get<"health_check_interval_seconds">(),
            "health_check_interval_seconds", "健康检查间隔不能为空", 1, 3600,
            "健康检查间隔必须在1到3600秒之间");
        require_bounded_field(validator, prefix, body.get<"health_check_timeout_seconds">(),
            "health_check_timeout_seconds", "健康检查超时不能为空", 1, 300,
            "健康检查超时必须在1到300秒之间");
        require_bounded_field(validator, prefix, body.get<"health_check_expected_status">(),
            "health_check_expected_status", "健康检查期望状态码不能为空", 100, 599,
            "健康检查期望状态码必须在100到599之间");
        require_bounded_field(validator, prefix, body.get<"healthy_threshold">(),
            "healthy_threshold", "健康恢复阈值不能为空", 1, 10, "健康恢复阈值必须在1到10之间");
        require_bounded_field(validator, prefix, body.get<"unhealthy_threshold">(),
            "unhealthy_threshold", "故障阈值不能为空", 1, 10, "故障阈值必须在1到10之间");
    }

    static void validate_access_log(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        require_field(validator, prefix, body.get<"access_log_enabled">(), "access_log_enabled",
            "访问日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_request_headers">(),
            "access_log_request_headers", "请求头日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_request_body">(),
            "access_log_request_body", "请求体日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_response_headers">(),
            "access_log_response_headers", "响应头日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_query_params">(),
            "access_log_query_params", "参数日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_cookies">(), "access_log_cookies",
            "Cookie 日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_referer">(), "access_log_referer",
            "请求来源日志设置不能为空");
        require_field(validator, prefix, body.get<"access_log_user_agent">(),
            "access_log_user_agent", "终端信息日志设置不能为空");
        const auto& access_log_status_code_ranges = body.get<"access_log_status_code_ranges">();
        require_field(validator, prefix, access_log_status_code_ranges,
            "access_log_status_code_ranges", "请选择访问日志状态码");
        add_if_invalid(validator, prefix, access_log_status_code_ranges,
            "access_log_status_code_ranges",
            [](const auto& v) { return valid_access_log_status_code_ranges(v); }, "custom",
            "访问日志状态码范围不正确");
        require_field(validator, prefix, body.get<"access_log_client_abort">(),
            "access_log_client_abort", "客户端中断日志设置不能为空");
    }

    static void validate_https(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        require_field(validator, prefix, body.get<"https_enabled">(), "https_enabled",
            "HTTPS 设置不能为空");
        const auto& certificate_ids = body.get<"certificate_ids">();
        require_field(validator, prefix, certificate_ids, "certificate_ids",
            "certificate_ids 不能为空");
        if (certificate_ids && certificate_ids->size() > max_website_certificates) {
            validator.add(field_path(prefix, "certificate_ids"), "too_big", "最多绑定20张证书");
        }
        add_if_invalid(validator, prefix, certificate_ids, "certificate_ids",
            [](const auto& v) { return has_unique_certificate_ids(v); }, "custom",
            "证书 ID 必须为 UUID 且不能重复");
        const auto& minimum_tls_version = body.get<"minimum_tls_version">();
        require_field(validator, prefix, minimum_tls_version, "minimum_tls_version",
            "请选择最低 TLS 版本");
        add_if_invalid(validator, prefix, minimum_tls_version, "minimum_tls_version",
            [](const auto& v) { return v.view() == "1.2" || v.view() == "1.3"; }, "regex",
            "最低 TLS 版本不正确");
        require_field(validator, prefix, body.get<"force_https">(), "force_https",
            "HTTPS 跳转设置不能为空");
        require_field(validator, prefix, body.get<"hsts_enabled">(), "hsts_enabled",
            "HSTS 设置不能为空");
    }

    static void validate_compression(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        require_field(validator, prefix, body.get<"response_compression_enabled">(),
            "response_compression_enabled", "响应压缩设置不能为空");
        const auto& response_compression_min_bytes = body.get<"response_compression_min_bytes">();
        require_bounded_field(validator, prefix, response_compression_min_bytes,
            "response_compression_min_bytes", "响应压缩阈值不能为空",
            static_cast<std::int64_t>(flexedge::node::min_response_compression_bytes),
            static_cast<std::int64_t>(flexedge::node::max_response_compression_min_bytes),
            "响应压缩阈值必须在256字节到1 MiB之间");
        const auto& response_compression_max_bytes = body.get<"response_compression_max_bytes">();
        require_field(validator, prefix, response_compression_max_bytes,
            "response_compression_max_bytes", "响应压缩最大长度不能为空");
        if (response_compression_max_bytes && response_compression_max_bytes->value_ < 0) {
            validator.add(field_path(prefix, "response_compression_max_bytes"), "too_small",
                "响应压缩最大长度不能小于0");
        }
        if (response_compression_max_bytes &&
            response_compression_max_bytes->value_ >
                static_cast<std::int64_t>(flexedge::node::max_response_compression_bytes)) {
            validator.add(field_path(prefix, "response_compression_max_bytes"), "too_big",
                "响应压缩最大长度不能超过64 MiB");
        }
        const auto& response_compression_algorithms = body.get<"response_compression_algorithms">();
        require_field(validator, prefix, response_compression_algorithms,
            "response_compression_algorithms", "压缩算法不能为空");
        add_if_invalid(validator, prefix, response_compression_algorithms,
            "response_compression_algorithms",
            [](const auto& v) { return valid_compression_algorithms(v); }, "custom",
            "压缩算法必须从 br、zstd、gzip 中选择且不能重复");
        const auto& response_compression_mime_types = body.get<"response_compression_mime_types">();
        require_field(validator, prefix, response_compression_mime_types,
            "response_compression_mime_types", "压缩 MIME 类型不能为空");
        add_if_invalid(validator, prefix, response_compression_mime_types,
            "response_compression_mime_types",
            [](const auto& v) {
                return valid_compression_values(v, flexedge::node::compression_match_kind::mime_type);
            },
            "custom", "压缩 MIME 类型不正确");
        const auto& response_compression_extensions = body.get<"response_compression_extensions">();
        require_field(validator, prefix, response_compression_extensions,
            "response_compression_extensions", "压缩扩展名不能为空");
        add_if_invalid(validator, prefix, response_compression_extensions,
            "response_compression_extensions",
            [](const auto& v) {
                return valid_compression_values(v, flexedge::node::compression_match_kind::extension);
            },
            "custom", "压缩扩展名不正确");
        const auto& response_compression_excluded_extensions =
            body.get<"response_compression_excluded_extensions">();
        require_field(validator, prefix, response_compression_excluded_extensions,
            "response_compression_excluded_extensions", "例外扩展名不能为空");
        add_if_invalid(validator, prefix, response_compression_excluded_extensions,
            "response_compression_excluded_extensions",
            [](const auto& v) {
                return valid_compression_values(v, flexedge::node::compression_match_kind::extension);
            },
            "custom", "例外扩展名不正确");
        if (response_compression_min_bytes && response_compression_max_bytes &&
            response_compression_max_bytes->value_ != 0 &&
            response_compression_max_bytes->value_ < response_compression_min_bytes->value_) {
            validator.add(field_path(prefix, "response_compression_max_bytes"), "range",
                "最大压缩大小不能小于最小压缩大小");
        }
    }

    static void validate_route_rules(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        const auto& rules = body.get<"route_rules">();
        require_field(validator, prefix, rules, "route_rules", "路由规则不能为空");
        if (!rules) {
            return;
        }
        if (rules->size() > flexedge::node::max_route_rules) {
            validator.add(field_path(prefix, "route_rules"), "too_big",
                "最多配置" + std::to_string(flexedge::node::max_route_rules) + "条路由规则");
        }
        add_if_invalid(validator, prefix, rules, "route_rules",
            [](const auto& v) { return valid_route_rule_set(v); }, "custom",
            "路由规则 ID 和请求头名称必须唯一");
        std::size_t index{};
        for (const auto& value : *rules) {
            website_route_rule_validator{}.validate_nested(
                value, field_path(prefix, "route_rules[" + std::to_string(index++) + "]"),
                validator);
        }
    }

    static void validate_origin_references(const website_config_input& body, std::string_view prefix,
        ruvia::validator& validator) {
        const auto& origins = body.get<"origins">();
        const auto& default_origin_group = body.get<"default_origin_group">();
        const auto& rules = body.get<"route_rules">();
        if (!origins || !default_origin_group || !rules) {
            return;
        }
        const auto enabled_origin_groups = collect_enabled_origin_groups(*origins);
        if (!enabled_origin_groups.contains(default_origin_group->view())) {
            validator.add(field_path(prefix, "default_origin_group"), "reference",
                "默认源站组必须包含启用的源站");
        }
        std::size_t index{};
        for (const auto& route : *rules) {
            const auto& action = route.get<"action">();
            const auto& origin_group = route.get<"origin_group">();
            if (action && origin_group) {
                const auto path = field_path(
                    prefix, "route_rules[" + std::to_string(index) + "].origin_group");
                const auto status = check_route_origin_reference(
                    action->view(), origin_group->view(), enabled_origin_groups);
                if (status == route_origin_reference_status::redirect_forbids_origin_group) {
                    validator.add(path, "forbidden", "跳转规则不能选择源站组");
                } else if (status == route_origin_reference_status::missing_enabled_origin_group) {
                    validator.add(path, "reference", "只能选择包含启用源站的源站组");
                }
            }
            ++index;
        }
    }
};

}  // namespace service::website_config
