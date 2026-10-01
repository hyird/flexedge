#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include "common/route_match.h"
#include "service/common/domain_name.h"
#include "service/common/uuid.h"
#include "service/common/ip_address.h"
#include "service/features/website_config/transport.h"
#include <ruvia/web/Validation.h>

namespace service::website_config {

inline constexpr std::size_t kMaxWebsiteCertificates{20};

inline bool isOriginHost(std::string_view value) {
    return service::common::isIpv4Address(value) || service::common::isHostname(value, false);
}

inline bool isOriginHostHeader(std::string_view value) {
    return value == "$host" || service::common::isHostname(value, false);
}

inline bool isWebsiteName(std::string_view value) { return !value.empty(); }

inline bool isOriginGroup(std::string_view value) {
    return !value.empty() && value.size() <= 100 &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::iscntrl(ch) != 0; });
}

inline bool validCompressionAlgorithms(const ruvia::Array<ruvia::String>& values) {
    if (values.empty() || values.size() > 3) {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if ((value.view() != "br" && value.view() != "zstd" && value.view() != "gzip") ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool validCompressionValues(const ruvia::Array<ruvia::String>& values) {
    if (values.size() > 32) {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if (value.empty() || value.view().size() > 127 ||
            std::ranges::any_of(value.view(),
                                [](unsigned char ch) { return std::isspace(ch) != 0; }) ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool isCompressionMimeType(std::string_view value) {
    const auto token = [](std::string_view segment) {
        return !segment.empty() && std::ranges::all_of(segment, [](unsigned char ch) {
            return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '&' ||
                   ch == '^' || ch == '_' || ch == '.' || ch == '+' || ch == '-';
        });
    };
    const auto slash = value.find('/');
    if (slash == std::string_view::npos || value.find('/', slash + 1) != std::string_view::npos) {
        return false;
    }
    const auto type = value.substr(0, slash);
    const auto subtype = value.substr(slash + 1);
    return token(type) && (subtype == "*" || token(subtype));
}

inline bool validCompressionMimeTypes(const ruvia::Array<ruvia::String>& values) {
    return validCompressionValues(values) && std::ranges::all_of(values, [](const auto& value) {
               return isCompressionMimeType(value.view());
           });
}

inline bool validCompressionExtensions(const ruvia::Array<ruvia::String>& values) {
    return validCompressionValues(values) && std::ranges::all_of(values, [](const auto& value) {
               return !value.empty() && value.view().front() == '.';
           });
}

inline bool validAccessLogStatusCodeRanges(const ruvia::Array<ruvia::String>& values) {
    if (values.empty() || values.size() > 5) {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if ((value.view() != "1xx" && value.view() != "2xx" && value.view() != "3xx" &&
             value.view() != "4xx" && value.view() != "5xx") ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool isHttpToken(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '%' || ch == '&' ||
               ch == '\'' || ch == '*' || ch == '+' || ch == '-' || ch == '.' || ch == '^' ||
               ch == '_' || ch == '`' || ch == '|' || ch == '~';
    });
}

inline bool isRouteHeaderName(std::string_view value) {
    if (!isHttpToken(value)) {
        return false;
    }
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return normalized != "connection" && normalized != "content-length" && normalized != "host" &&
           normalized != "keep-alive" && normalized != "proxy-connection" &&
           normalized != "strict-transport-security" && normalized != "te" &&
           normalized != "trailer" && normalized != "transfer-encoding" && normalized != "upgrade";
}

inline bool isRouteHeaderValue(std::string_view value) {
    return value.size() <= 4096 && value.find('\r') == std::string_view::npos &&
           value.find('\n') == std::string_view::npos;
}

inline bool isRoutePath(std::string_view value) {
    return value.size() <= 2048 && !value.empty() && value.front() == '/' &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool isRedirectUrl(std::string_view value) {
    return value.size() <= 2048 && !value.empty() &&
           (value.front() == '/' || value.starts_with("http://") ||
            value.starts_with("https://")) &&
           std::ranges::none_of(value, [](unsigned char ch) { return std::isspace(ch) != 0; }) &&
           value.find('\r') == std::string_view::npos && value.find('\n') == std::string_view::npos;
}

inline bool validRouteMethods(const ruvia::Array<ruvia::String>& values) {
    if (values.size() > 8) {
        return false;
    }
    std::unordered_set<std::string_view> seen;
    for (const auto& value : values) {
        if ((value.view() != "GET" && value.view() != "HEAD" && value.view() != "POST" &&
             value.view() != "PUT" && value.view() != "PATCH" && value.view() != "DELETE" &&
             value.view() != "OPTIONS") ||
            !seen.emplace(value.view()).second) {
            return false;
        }
    }
    return true;
}

inline bool validRouteHeaderSet(const ruvia::Array<WebsiteRouteHeaderInput>& headers) {
    std::unordered_set<std::string> names;
    for (const auto& header : headers) {
        const auto& name = header.get<"name">();
        const auto& value = header.get<"value">();
        if (!name || !value || !isRouteHeaderName(name->view()) ||
            !isRouteHeaderValue(value->view())) {
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

struct WebsiteDomainValidator final {
    template <typename ValidatorT>
    void validateNested(const WebsiteDomainInput& value, std::string_view path,
                        ValidatorT& validator) const {
        const auto& id = value.get<"id">();
        if (!id || !service::common::parseUuid(id ? std::optional<std::string_view>{id->view()}
                                                  : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "域名 ID 不正确");
        }
        const auto& hostname = value.get<"hostname">();
        if (!hostname || hostname->empty()) {
            validator.add(std::string(path) + ".hostname", "required", "域名不能为空");
        } else if (!service::common::isHostname(hostname->view())) {
            validator.add(std::string(path) + ".hostname", "format", "域名格式不正确");
        }
        const auto& mode = value.get<"dnsMode">();
        if (!mode || (mode->view() != "managed" && mode->view() != "external")) {
            validator.add(std::string(path) + ".dns_mode", "enum", "解析方式不正确");
        }
    }
};

struct WebsiteOriginValidator final {
    template <typename ValidatorT>
    void validateNested(const WebsiteOriginInput& value, std::string_view path,
                        ValidatorT& validator) const {
        const auto& id = value.get<"id">();
        if (!id || !service::common::parseUuid(id ? std::optional<std::string_view>{id->view()}
                                                  : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "源站 ID 不正确");
        }
        const auto& group = value.get<"group">();
        if (!group || !isOriginGroup(group->view())) {
            validator.add(std::string(path) + ".group", "format", "源站组名称不正确");
        }
        const auto& protocol = value.get<"protocol">();
        if (!protocol || (protocol->view() != "http" && protocol->view() != "https")) {
            validator.add(std::string(path) + ".protocol", "enum", "源站协议不正确");
        }
        const auto& host = value.get<"host">();
        if (!host || host->empty()) {
            validator.add(std::string(path) + ".host", "required", "源站地址不能为空");
        } else if (host->size() > 253 || !isOriginHost(host->view())) {
            validator.add(std::string(path) + ".host", "format", "源站地址格式不正确");
        }
        if (const auto& port = value.get<"port">();
            !port || port->value < 1 || port->value > 65535) {
            validator.add(std::string(path) + ".port", "range", "源站端口不正确");
        }
        const auto& role = value.get<"role">();
        if (!role || (role->view() != "primary" && role->view() != "backup")) {
            validator.add(std::string(path) + ".role", "enum", "源站角色不正确");
        }
        if (const auto& weight = value.get<"weight">();
            !weight || weight->value < 1 || weight->value > 100) {
            validator.add(std::string(path) + ".weight", "range", "源站权重不正确");
        }
        const auto& status = value.get<"status">();
        if (!status || (status->view() != "enabled" && status->view() != "disabled")) {
            validator.add(std::string(path) + ".status", "enum", "源站状态不正确");
        }
    }
};

struct WebsiteRouteHeaderValidator final {
    template <typename ValidatorT>
    void validateNested(const WebsiteRouteHeaderInput& value, std::string_view path,
                        ValidatorT& validator) const {
        const auto& name = value.get<"name">();
        if (!name || !isRouteHeaderName(name->view())) {
            validator.add(std::string(path) + ".name", "format", "请求头名称不正确或不允许修改");
        }
        const auto& headerValue = value.get<"value">();
        if (!headerValue || !isRouteHeaderValue(headerValue->view())) {
            validator.add(std::string(path) + ".value", "format", "请求头值不正确");
        }
    }
};

struct WebsiteRouteRuleValidator final {
    template <typename ValidatorT>
    void validateNested(const WebsiteRouteRuleInput& value, std::string_view path,
                        ValidatorT& validator) const {
        namespace policy = flexedge::route_policy;
        if (const auto& name = value.get<"name">(); name && !policy::cleanText(name->view(), 100)) {
            validator.add(std::string(path) + ".name", "format",
                          "规则名称最多100字节且不能包含控制字符");
        }
        if (const auto& description = value.get<"description">();
            description && description->view().size() > 1000) {
            validator.add(std::string(path) + ".description", "format", "规则备注最多1000字节");
        }
        if (const auto& hostnames = value.get<"hostnames">()) {
            std::unordered_set<std::string> seen;
            if (hostnames->size() > 100)
                validator.add(std::string(path) + ".hostnames", "format", "最多100个匹配域名");
            for (const auto& host : *hostnames) {
                if (!policy::exactHostname(host.view()) ||
                    !seen.emplace(policy::hostname(host.view())).second) {
                    validator.add(std::string(path) + ".hostnames", "format",
                                  "请填写不重复的精确域名，不含协议、端口或通配符");
                    break;
                }
            }
        }
        const auto& id = value.get<"id">();
        if (!id || !service::common::parseUuid(id ? std::optional<std::string_view>{id->view()}
                                                  : std::nullopt)) {
            validator.add(std::string(path) + ".id", "format", "规则 ID 不正确");
        }
        const auto& status = value.get<"status">();
        if (!status || (status->view() != "enabled" && status->view() != "disabled")) {
            validator.add(std::string(path) + ".status", "enum", "规则状态不正确");
        }
        const auto& matchType = value.get<"matchType">();
        if (!matchType || !policy::validMatchType(matchType->view())) {
            validator.add(std::string(path) + ".match_type", "enum", "匹配方式不正确");
        }
        const auto& routePath = value.get<"path">();
        std::shared_ptr<const RE2> pattern;
        if (matchType && matchType->view() == "regex") {
            if (routePath)
                pattern = policy::compilePattern(routePath->view());
            if (!pattern)
                validator.add(
                    std::string(path) + ".path", "format",
                    "正则需符合 RE2 语法，最多512字节、9个捕获组，不支持回溯引用、前后查找或 \\C");
        } else if (matchType && matchType->view() == "suffix") {
            if (!routePath || routePath->empty() || !policy::cleanText(routePath->view(), 2048) ||
                routePath->view().find_first_of(" ?#") != std::string_view::npos)
                validator.add(std::string(path) + ".path", "format",
                              "请填写路径后缀，不含空白、查询参数或片段");
        } else if (!routePath || !isRoutePath(routePath->view())) {
            validator.add(std::string(path) + ".path", "format", "匹配路径必须以 / 开头");
        }
        if (const auto& conditions = value.get<"conditions">()) {
            if (conditions->size() > 20)
                validator.add(std::string(path) + ".conditions", "format", "最多20个匹配条件");
            std::size_t index = 0;
            for (const auto& condition : *conditions) {
                const auto& source = condition.get<"source">();
                const auto& name = condition.get<"name">();
                const auto& op = condition.get<"op">();
                const auto& text = condition.get<"value">();
                if (!source || !name || !op || !text ||
                    !policy::conditionOptions(source->view(), name->view(), op->view(),
                                              text->view()))
                    validator.add(std::string(path) + ".conditions." + std::to_string(index) +
                                      ".name",
                                  "format", "条件来源、名称、操作符或值不正确");
                ++index;
            }
        }
        const auto& methods = value.get<"methods">();
        if (!methods || !validRouteMethods(*methods)) {
            validator.add(std::string(path) + ".methods", "format", "请求方法不正确或重复");
        }
        const auto& action = value.get<"action">();
        if (!action || (action->view() != "proxy" && action->view() != "redirect" &&
                        action->view() != "rewrite")) {
            validator.add(std::string(path) + ".action", "enum", "规则动作不正确");
            return;
        }
        const auto& rewritePath = value.get<"rewritePath">();
        const auto& redirectUrl = value.get<"redirectUrl">();
        const auto& redirectStatus = value.get<"redirectStatus">();
        const auto& originGroup = value.get<"originGroup">();
        if (!rewritePath || !redirectUrl || !redirectStatus || !originGroup) {
            return;
        }
        if (pattern &&
            (!policy::captureTemplate(rewritePath->view(), pattern->NumberOfCapturingGroups()) ||
             !policy::captureTemplate(redirectUrl->view(), pattern->NumberOfCapturingGroups()))) {
            validator.add(std::string(path) +
                              (action->view() == "proxy" ? ".rewrite_path" : ".redirect_url"),
                          "format", "捕获替换仅支持正则中存在的 ${0} 到 ${9}，字面美元符用 $$");
        }
        const auto& configuredMode = value.get<"rewriteMode">();
        const auto mode = configuredMode ? configuredMode->view() : std::string_view{};
        const auto& configuredQuery = value.get<"queryMode">();
        const auto queryMode = configuredQuery ? configuredQuery->view() : std::string_view{};
        const auto& query = value.get<"queryString">();
        const auto queryText = query ? query->view() : std::string_view{};
        if (!policy::rewriteMode(mode))
            validator.add(std::string(path) + ".rewrite_mode", "enum", "重写方式不正确");
        if (!policy::queryMode(queryMode))
            validator.add(std::string(path) + ".query_mode", "enum", "查询参数策略不正确");
        if (!policy::queryString(queryText) || (queryMode != "replace" && !queryText.empty())) {
            validator.add(std::string(path) + ".query_string", "format",
                          "仅替换策略可填写参数，不含开头问号、空白或片段标记");
        }
        if ((mode == "none" || mode == "strip_prefix") && !rewritePath->empty()) {
            validator.add(std::string(path) + ".rewrite_path", "format",
                          "当前重写方式无需填写替换路径");
        }
        if ((mode == "replace_path" || mode == "replace_prefix") && rewritePath->empty()) {
            validator.add(std::string(path) + ".rewrite_path", "required", "请填写替换路径");
        }
        if ((mode == "strip_prefix" || mode == "replace_prefix") &&
            (!matchType || matchType->view() != "prefix" || !routePath ||
             !policy::pathOnly(routePath->view()) ||
             (mode == "replace_prefix" && !policy::pathOnly(rewritePath->view())))) {
            validator.add(std::string(path) + ".rewrite_mode", "format",
                          "前缀重写仅适用于前缀匹配，路径不能含查询参数或片段");
        }
        if (action->view() == "redirect" && !mode.empty() && mode != "none") {
            validator.add(std::string(path) + ".rewrite_mode", "format",
                          "跳转规则不能同时重写回源路径");
        }
        if (action->view() == "rewrite") {
            const auto& requestHeaders = value.get<"requestHeaders">();
            const auto& responseHeaders = value.get<"responseHeaders">();
            if ((!rewritePath->empty() && !policy::pathOnly(rewritePath->view())) ||
                !redirectUrl->empty() || redirectStatus->value != 0 || !originGroup->empty() ||
                (requestHeaders && !requestHeaders->empty()) ||
                (responseHeaders && !responseHeaders->empty()))
                validator.add(std::string(path), "format",
                              "重写规则不能配置源站、跳转或请求头，重写路径必须以 / 开头");
        } else if (action->view() == "proxy") {
            if ((!rewritePath->empty() && !policy::pathOnly(rewritePath->view())) ||
                !redirectUrl->empty() || redirectStatus->value != 0 ||
                !isOriginGroup(originGroup->view())) {
                validator.add(std::string(path), "format", "代理规则配置不正确");
            }
        } else if (!isRedirectUrl(redirectUrl->view()) ||
                   !policy::redirectStatus(redirectStatus->value) || !rewritePath->empty() ||
                   !originGroup->empty()) {
            validator.add(std::string(path), "format", "重定向规则配置不正确");
        }
    }
};

inline bool validDomainSet(const ruvia::Array<WebsiteDomainInput>& domains) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> hostnames;
    for (const auto& domain : domains) {
        const auto& id = domain.get<"id">();
        const auto parsedId = service::common::parseUuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& hostname = domain.get<"hostname">();
        if (!parsedId || !hostname || !ids.insert(*parsedId).second) {
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

inline bool validOriginSet(const ruvia::Array<WebsiteOriginInput>& origins) {
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string_view> enabledGroups;
    std::unordered_set<std::string_view> enabledPrimaryGroups;
    for (const auto& origin : origins) {
        const auto& id = origin.get<"id">();
        const auto parsedId = service::common::parseUuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& group = origin.get<"group">();
        if (!parsedId || !group || !ids.insert(*parsedId).second) {
            return false;
        }
        const auto& role = origin.get<"role">();
        const auto& status = origin.get<"status">();
        if (status && status->view() == "enabled") {
            enabledGroups.emplace(group->view());
            if (role && role->view() == "primary") {
                enabledPrimaryGroups.emplace(group->view());
            }
        }
    }
    return !enabledGroups.empty() && enabledGroups == enabledPrimaryGroups;
}

inline bool validRouteRuleSet(const ruvia::Array<WebsiteRouteRuleInput>& rules) {
    std::unordered_set<std::string> ids;
    for (const auto& rule : rules) {
        const auto& id = rule.get<"id">();
        const auto parsedId = service::common::parseUuid(
            id ? std::optional<std::string_view>{id->view()} : std::nullopt);
        const auto& requestHeaders = rule.get<"requestHeaders">();
        const auto& responseHeaders = rule.get<"responseHeaders">();
        const auto& originGroup = rule.get<"originGroup">();
        if (!parsedId || !requestHeaders || !responseHeaders || !ids.insert(*parsedId).second ||
            !validRouteHeaderSet(*requestHeaders) || !validRouteHeaderSet(*responseHeaders)) {
            return false;
        }
        if (!originGroup) {
            return false;
        }
    }
    return true;
}

inline bool hasUniqueCertificateIds(const ruvia::Array<ruvia::String>& values) {
    std::unordered_set<std::string> ids;
    for (const auto& value : values) {
        const auto id = service::common::parseUuid(value.view());
        if (!id || !ids.insert(*id).second) {
            return false;
        }
    }
    return true;
}

struct WebsiteConfigRules final {
    void validateNested(const WebsiteConfigInput& body, std::string_view prefix,
                        ruvia::Validator& validator) const {
        const auto field = [&](std::string_view name) {
            std::string result(prefix);
            if (!result.empty())
                result.push_back('.');
            result.append(name);
            return result;
        };
        const auto required = [&](const auto& value, std::string_view name,
                                  std::string_view message) {
            validator.required(value, field(name), message);
        };
        const auto bounded = [&](const auto& value, std::string_view name, auto minimum,
                                 auto maximum, std::string_view message) {
            validator.range(value, field(name), minimum, maximum, message);
        };
        const auto custom = [&](const auto& value, std::string_view name, auto predicate,
                                std::string_view code, std::string_view message) {
            if (value && !predicate(*value))
                validator.add(field(name), code, message);
        };
        const auto& name = body.get<"name">();
        validator.maxLength(name, field("name"), 100, "网站名称最多100个字符");
        custom(
            name, "name", [](const auto& v) { return isWebsiteName(v.view()); }, "custom",
            "网站名称不能为空");

        const auto& domains = body.get<"domains">();
        required(domains, "domains", "至少需要一个绑定域名");
        if (domains) {
            if (domains->empty())
                validator.add(field("domains"), "too_small", "至少需要一个绑定域名");
            if (domains->size() > 100)
                validator.add(field("domains"), "too_big", "单个网站最多绑定100个域名");
            custom(
                domains, "domains", [](const auto& v) { return validDomainSet(v); }, "custom",
                "域名 ID 和主机名必须唯一");
            std::size_t index{};
            for (const auto& value : *domains) {
                WebsiteDomainValidator{}.validateNested(
                    value, field("domains[" + std::to_string(index++) + "]"), validator);
            }
        }
        const auto& origins = body.get<"origins">();
        required(origins, "origins", "至少需要一个源站");
        if (origins) {
            if (origins->empty())
                validator.add(field("origins"), "too_small", "至少需要一个源站");
            if (origins->size() > 100)
                validator.add(field("origins"), "too_big", "单个网站最多配置100个源站");
            custom(
                origins, "origins", [](const auto& v) { return validOriginSet(v); }, "custom",
                "源站 ID 必须唯一，且每个启用的源站组都需要主源站");
            std::size_t index{};
            for (const auto& value : *origins) {
                WebsiteOriginValidator{}.validateNested(
                    value, field("origins[" + std::to_string(index++) + "]"), validator);
            }
        }

        const auto& defaultOriginGroup = body.get<"defaultOriginGroup">();
        required(defaultOriginGroup, "default_origin_group", "请选择默认源站组");
        validator.maxLength(defaultOriginGroup, field("default_origin_group"), 100,
                            "默认源站组最多100个字符");
        custom(
            defaultOriginGroup, "default_origin_group",
            [](const auto& v) { return isOriginGroup(v.view()); }, "custom",
            "默认源站组名称不正确");
        const auto& originHostHeader = body.get<"originHostHeader">();
        required(originHostHeader, "origin_host_header", "回源 Host 不能为空");
        validator.maxLength(originHostHeader, field("origin_host_header"), 253,
                            "回源 Host 最多253个字符");
        custom(
            originHostHeader, "origin_host_header",
            [](const auto& v) { return isOriginHostHeader(v.view()); }, "custom",
            "请输入 $host 或有效域名");

        const auto& originConnectTimeoutSeconds = body.get<"originConnectTimeoutSeconds">();
        required(originConnectTimeoutSeconds, "origin_connect_timeout_seconds", "连接超时不能为空");
        bounded(originConnectTimeoutSeconds, "origin_connect_timeout_seconds", 1, 300,
                "连接超时必须在1到300秒之间");
        const auto& originReadTimeoutSeconds = body.get<"originReadTimeoutSeconds">();
        required(originReadTimeoutSeconds, "origin_read_timeout_seconds", "读取超时不能为空");
        bounded(originReadTimeoutSeconds, "origin_read_timeout_seconds", 1, 600,
                "读取超时必须在1到600秒之间");
        required(body.get<"passClientIp">(), "pass_client_ip", "真实访客 IP 设置不能为空");
        required(body.get<"healthCheckEnabled">(), "health_check_enabled", "健康检查设置不能为空");
        const auto& healthCheckPath = body.get<"healthCheckPath">();
        required(healthCheckPath, "health_check_path", "健康检查路径不能为空");
        custom(
            healthCheckPath, "health_check_path",
            [](const auto& v) { return isRoutePath(v.view()); }, "custom",
            "健康检查路径必须以 / 开头");
        const auto& healthCheckIntervalSeconds = body.get<"healthCheckIntervalSeconds">();
        required(healthCheckIntervalSeconds, "health_check_interval_seconds",
                 "健康检查间隔不能为空");
        bounded(healthCheckIntervalSeconds, "health_check_interval_seconds", 1, 3600,
                "健康检查间隔必须在1到3600秒之间");
        const auto& healthCheckTimeoutSeconds = body.get<"healthCheckTimeoutSeconds">();
        required(healthCheckTimeoutSeconds, "health_check_timeout_seconds", "健康检查超时不能为空");
        bounded(healthCheckTimeoutSeconds, "health_check_timeout_seconds", 1, 300,
                "健康检查超时必须在1到300秒之间");
        const auto& healthCheckExpectedStatus = body.get<"healthCheckExpectedStatus">();
        required(healthCheckExpectedStatus, "health_check_expected_status",
                 "健康检查期望状态码不能为空");
        bounded(healthCheckExpectedStatus, "health_check_expected_status", 100, 599,
                "健康检查期望状态码必须在100到599之间");
        const auto& healthyThreshold = body.get<"healthyThreshold">();
        required(healthyThreshold, "healthy_threshold", "健康恢复阈值不能为空");
        bounded(healthyThreshold, "healthy_threshold", 1, 10, "健康恢复阈值必须在1到10之间");
        const auto& unhealthyThreshold = body.get<"unhealthyThreshold">();
        required(unhealthyThreshold, "unhealthy_threshold", "故障阈值不能为空");
        bounded(unhealthyThreshold, "unhealthy_threshold", 1, 10, "故障阈值必须在1到10之间");

        required(body.get<"accessLogEnabled">(), "access_log_enabled", "访问日志设置不能为空");
        required(body.get<"accessLogRequestHeaders">(), "access_log_request_headers",
                 "请求头日志设置不能为空");
        required(body.get<"accessLogRequestBody">(), "access_log_request_body",
                 "请求体日志设置不能为空");
        required(body.get<"accessLogResponseHeaders">(), "access_log_response_headers",
                 "响应头日志设置不能为空");
        required(body.get<"accessLogQueryParams">(), "access_log_query_params",
                 "参数日志设置不能为空");
        required(body.get<"accessLogCookies">(), "access_log_cookies", "Cookie 日志设置不能为空");
        required(body.get<"accessLogReferer">(), "access_log_referer", "请求来源日志设置不能为空");
        required(body.get<"accessLogUserAgent">(), "access_log_user_agent",
                 "终端信息日志设置不能为空");
        const auto& accessLogStatusCodeRanges = body.get<"accessLogStatusCodeRanges">();
        required(accessLogStatusCodeRanges, "access_log_status_code_ranges",
                 "请选择访问日志状态码");
        custom(
            accessLogStatusCodeRanges, "access_log_status_code_ranges",
            [](const auto& v) { return validAccessLogStatusCodeRanges(v); }, "custom",
            "访问日志状态码范围不正确");
        required(body.get<"accessLogClientAbort">(), "access_log_client_abort",
                 "客户端中断日志设置不能为空");

        required(body.get<"httpsEnabled">(), "https_enabled", "HTTPS 设置不能为空");
        const auto& certificateIds = body.get<"certificateIds">();
        required(certificateIds, "certificate_ids", "certificate_ids 不能为空");
        if (certificateIds && certificateIds->size() > kMaxWebsiteCertificates)
            validator.add(field("certificate_ids"), "too_big", "最多绑定20张证书");
        custom(
            certificateIds, "certificate_ids",
            [](const auto& v) { return hasUniqueCertificateIds(v); }, "custom",
            "证书 ID 必须为 UUID 且不能重复");
        const auto& minimumTlsVersion = body.get<"minimumTlsVersion">();
        required(minimumTlsVersion, "minimum_tls_version", "请选择最低 TLS 版本");
        custom(
            minimumTlsVersion, "minimum_tls_version",
            [](const auto& v) { return v.view() == "1.2" || v.view() == "1.3"; }, "regex",
            "最低 TLS 版本不正确");
        required(body.get<"forceHttps">(), "force_https", "HTTPS 跳转设置不能为空");
        required(body.get<"http2Enabled">(), "http2_enabled", "HTTP/2 设置不能为空");
        required(body.get<"hstsEnabled">(), "hsts_enabled", "HSTS 设置不能为空");
        required(body.get<"responseCompressionEnabled">(), "response_compression_enabled",
                 "响应压缩设置不能为空");
        const auto& responseCompressionMinBytes = body.get<"responseCompressionMinBytes">();
        required(responseCompressionMinBytes, "response_compression_min_bytes",
                 "响应压缩阈值不能为空");
        bounded(responseCompressionMinBytes, "response_compression_min_bytes", 256, 1048576,
                "响应压缩阈值必须在256字节到1 MiB之间");
        const auto& responseCompressionMaxBytes = body.get<"responseCompressionMaxBytes">();
        required(responseCompressionMaxBytes, "response_compression_max_bytes",
                 "响应压缩最大长度不能为空");
        if (responseCompressionMaxBytes && responseCompressionMaxBytes->value < 0)
            validator.add(field("response_compression_max_bytes"), "too_small",
                          "响应压缩最大长度不能小于0");
        if (responseCompressionMaxBytes && responseCompressionMaxBytes->value > 67108864)
            validator.add(field("response_compression_max_bytes"), "too_big",
                          "响应压缩最大长度不能超过64 MiB");
        const auto& responseCompressionAlgorithms = body.get<"responseCompressionAlgorithms">();
        required(responseCompressionAlgorithms, "response_compression_algorithms",
                 "压缩算法不能为空");
        custom(
            responseCompressionAlgorithms, "response_compression_algorithms",
            [](const auto& v) { return validCompressionAlgorithms(v); }, "custom",
            "压缩算法必须从 br、zstd、gzip 中选择且不能重复");
        const auto& responseCompressionMimeTypes = body.get<"responseCompressionMimeTypes">();
        required(responseCompressionMimeTypes, "response_compression_mime_types",
                 "压缩 MIME 类型不能为空");
        custom(
            responseCompressionMimeTypes, "response_compression_mime_types",
            [](const auto& v) { return validCompressionMimeTypes(v); }, "custom",
            "压缩 MIME 类型不正确");
        const auto& responseCompressionExtensions = body.get<"responseCompressionExtensions">();
        required(responseCompressionExtensions, "response_compression_extensions",
                 "压缩扩展名不能为空");
        custom(
            responseCompressionExtensions, "response_compression_extensions",
            [](const auto& v) { return validCompressionExtensions(v); }, "custom",
            "压缩扩展名不正确");
        const auto& responseCompressionExcludedExtensions =
            body.get<"responseCompressionExcludedExtensions">();
        required(responseCompressionExcludedExtensions, "response_compression_excluded_extensions",
                 "例外扩展名不能为空");
        custom(
            responseCompressionExcludedExtensions, "response_compression_excluded_extensions",
            [](const auto& v) { return validCompressionExtensions(v); }, "custom",
            "例外扩展名不正确");
        const auto& rules = body.get<"routeRules">();
        required(rules, "route_rules", "路由规则不能为空");
        if (rules) {
            if (rules->size() > 100)
                validator.add(field("route_rules"), "too_big", "最多配置100条路由规则");
            custom(
                rules, "route_rules", [](const auto& v) { return validRouteRuleSet(v); }, "custom",
                "路由规则 ID 和请求头名称必须唯一");
            std::size_t index{};
            for (const auto& value : *rules) {
                WebsiteRouteRuleValidator{}.validateNested(
                    value, field("route_rules[" + std::to_string(index++) + "]"), validator);
            }
        }

        if (responseCompressionMinBytes && responseCompressionMaxBytes &&
            responseCompressionMaxBytes->value != 0 &&
            responseCompressionMaxBytes->value < responseCompressionMinBytes->value) {
            validator.add(field("response_compression_max_bytes"), "range",
                          "最大压缩大小不能小于最小压缩大小");
        }
        if (!origins || !defaultOriginGroup || !rules)
            return;
        std::unordered_set<std::string_view> enabledOriginGroups;
        for (const auto& origin : *origins) {
            const auto& group = origin.get<"group">();
            const auto& status = origin.get<"status">();
            if (group && status && status->view() == "enabled")
                enabledOriginGroups.emplace(group->view());
        }
        if (!enabledOriginGroups.contains(defaultOriginGroup->view()))
            validator.add(field("default_origin_group"), "reference",
                          "默认源站组必须包含启用的源站");
        std::size_t index{};
        for (const auto& route : *rules) {
            const auto& action = route.get<"action">();
            const auto& originGroup = route.get<"originGroup">();
            if (action && originGroup) {
                const auto path = field("route_rules[" + std::to_string(index) + "].origin_group");
                if (action->view() == "redirect" && !originGroup->empty())
                    validator.add(path, "forbidden", "跳转规则不能选择源站组");
                if (action->view() == "proxy" && !enabledOriginGroups.contains(originGroup->view()))
                    validator.add(path, "reference", "只能选择包含启用源站的源站组");
            }
            ++index;
        }
    }
};

} // namespace service::website_config
