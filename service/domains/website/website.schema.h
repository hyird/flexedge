#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

#include <ruvia/web/Controller.h>

#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/common/ip_address.h"
#include "service/domains/website/website.types.h"

namespace service::website {

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

inline bool
validRouteHeaderSet(const ruvia::Array<service::website_config::WebsiteRouteHeaderInput>& headers) {
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
    void validateNested(const service::website_config::WebsiteDomainInput& value,
                        std::string_view path, ValidatorT& validator) const {
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
    void validateNested(const service::website_config::WebsiteOriginInput& value,
                        std::string_view path, ValidatorT& validator) const {
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
    void validateNested(const service::website_config::WebsiteRouteHeaderInput& value,
                        std::string_view path, ValidatorT& validator) const {
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
    void validateNested(const service::website_config::WebsiteRouteRuleInput& value,
                        std::string_view path, ValidatorT& validator) const {
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
        if (!matchType || (matchType->view() != "exact" && matchType->view() != "prefix")) {
            validator.add(std::string(path) + ".match_type", "enum", "匹配方式不正确");
        }
        const auto& routePath = value.get<"path">();
        if (!routePath || !isRoutePath(routePath->view())) {
            validator.add(std::string(path) + ".path", "format", "匹配路径必须以 / 开头");
        }
        const auto& methods = value.get<"methods">();
        if (!methods || !validRouteMethods(*methods)) {
            validator.add(std::string(path) + ".methods", "format", "请求方法不正确或重复");
        }
        const auto& action = value.get<"action">();
        if (!action || (action->view() != "proxy" && action->view() != "redirect")) {
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
        if (action->view() == "proxy") {
            if ((!rewritePath->empty() && !isRoutePath(rewritePath->view())) ||
                !redirectUrl->empty() || redirectStatus->value != 0 ||
                !isOriginGroup(originGroup->view())) {
                validator.add(std::string(path), "format", "代理规则配置不正确");
            }
        } else if (!isRedirectUrl(redirectUrl->view()) ||
                   (redirectStatus->value != 301 && redirectStatus->value != 302) ||
                   !rewritePath->empty() || !originGroup->empty()) {
            validator.add(std::string(path), "format", "重定向规则配置不正确");
        }
    }
};

inline bool
validDomainSet(const ruvia::Array<service::website_config::WebsiteDomainInput>& domains) {
    std::unordered_set<std::string_view> ids;
    std::unordered_set<std::string> hostnames;
    for (const auto& domain : domains) {
        const auto& id = domain.get<"id">();
        const auto& hostname = domain.get<"hostname">();
        if (!id || !hostname || !ids.insert(id->view()).second) {
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

inline bool
validOriginSet(const ruvia::Array<service::website_config::WebsiteOriginInput>& origins) {
    std::unordered_set<std::string_view> ids;
    std::unordered_set<std::string_view> enabledGroups;
    std::unordered_set<std::string_view> enabledPrimaryGroups;
    for (const auto& origin : origins) {
        const auto& id = origin.get<"id">();
        const auto& group = origin.get<"group">();
        if (!id || !group || !ids.insert(id->view()).second) {
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

inline bool
validRouteRuleSet(const ruvia::Array<service::website_config::WebsiteRouteRuleInput>& rules) {
    std::unordered_set<std::string_view> ids;
    for (const auto& rule : rules) {
        const auto& id = rule.get<"id">();
        const auto& requestHeaders = rule.get<"requestHeaders">();
        const auto& responseHeaders = rule.get<"responseHeaders">();
        const auto& originGroup = rule.get<"originGroup">();
        if (!id || !requestHeaders || !responseHeaders || !ids.emplace(id->view()).second ||
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
    std::unordered_set<std::string_view> ids;
    for (const auto& value : values) {
        if (!service::common::parseUuid(std::optional<std::string_view>{value.view()}) ||
            !ids.insert(value.view()).second) {
            return false;
        }
    }
    return true;
}

struct WebsiteConfigRules final {
    void validateNested(const service::website_config::WebsiteConfigInput& body,
                        std::string_view prefix, ruvia::Validator& validator) const {
        RUVIA_VALIDATION_FOR_EACH(
            RUVIA_VALIDATE_RULE_FIELD, service::website_config::WebsiteConfigInput,
            RUVIA_RULE(name, RUVIA_MAX(100, "网站名称最多100个字符"),
                       RUVIA_CUSTOM("网站名称不能为空", isWebsiteName)),
            RUVIA_RULE(domains, RUVIA_REQUIRED("至少需要一个绑定域名"),
                       RUVIA_MIN(1, "至少需要一个绑定域名"),
                       RUVIA_MAX(100, "单个网站最多绑定100个域名"),
                       RUVIA_CUSTOM("域名 ID 和主机名必须唯一", validDomainSet),
                       RUVIA_EACH(WebsiteDomainValidator)),
            RUVIA_RULE(
                origins, RUVIA_REQUIRED("至少需要一个源站"), RUVIA_MIN(1, "至少需要一个源站"),
                RUVIA_MAX(100, "单个网站最多配置100个源站"),
                RUVIA_CUSTOM("源站 ID 必须唯一，且每个启用的源站组都需要主源站", validOriginSet),
                RUVIA_EACH(WebsiteOriginValidator)),
            RUVIA_RULE_NAME("default_origin_group", defaultOriginGroup,
                            RUVIA_REQUIRED("请选择默认源站组"),
                            RUVIA_MAX(100, "默认源站组最多100个字符"),
                            RUVIA_CUSTOM("默认源站组名称不正确", isOriginGroup)),
            RUVIA_RULE_NAME("origin_host_header", originHostHeader,
                            RUVIA_REQUIRED("回源 Host 不能为空"),
                            RUVIA_MAX(253, "回源 Host 最多253个字符"),
                            RUVIA_CUSTOM("请输入 $host 或有效域名", isOriginHostHeader)),
            RUVIA_RULE_NAME("origin_connect_timeout_seconds", originConnectTimeoutSeconds,
                            RUVIA_REQUIRED("连接超时不能为空"),
                            RUVIA_MIN(1, "连接超时必须在1到300秒之间"),
                            RUVIA_MAX(300, "连接超时必须在1到300秒之间")),
            RUVIA_RULE_NAME("origin_read_timeout_seconds", originReadTimeoutSeconds,
                            RUVIA_REQUIRED("读取超时不能为空"),
                            RUVIA_MIN(1, "读取超时必须在1到600秒之间"),
                            RUVIA_MAX(600, "读取超时必须在1到600秒之间")),
            RUVIA_RULE_NAME("pass_client_ip", passClientIp,
                            RUVIA_REQUIRED("真实访客 IP 设置不能为空")),
            RUVIA_RULE_NAME("health_check_enabled", healthCheckEnabled,
                            RUVIA_REQUIRED("健康检查设置不能为空")),
            RUVIA_RULE_NAME("health_check_path", healthCheckPath,
                            RUVIA_REQUIRED("健康检查路径不能为空"),
                            RUVIA_CUSTOM("健康检查路径必须以 / 开头", isRoutePath)),
            RUVIA_RULE_NAME("health_check_interval_seconds", healthCheckIntervalSeconds,
                            RUVIA_REQUIRED("健康检查间隔不能为空"),
                            RUVIA_MIN(1, "健康检查间隔必须在1到3600秒之间"),
                            RUVIA_MAX(3600, "健康检查间隔必须在1到3600秒之间")),
            RUVIA_RULE_NAME("health_check_timeout_seconds", healthCheckTimeoutSeconds,
                            RUVIA_REQUIRED("健康检查超时不能为空"),
                            RUVIA_MIN(1, "健康检查超时必须在1到300秒之间"),
                            RUVIA_MAX(300, "健康检查超时必须在1到300秒之间")),
            RUVIA_RULE_NAME("health_check_expected_status", healthCheckExpectedStatus,
                            RUVIA_REQUIRED("健康检查期望状态码不能为空"),
                            RUVIA_MIN(100, "健康检查期望状态码必须在100到599之间"),
                            RUVIA_MAX(599, "健康检查期望状态码必须在100到599之间")),
            RUVIA_RULE_NAME("healthy_threshold", healthyThreshold,
                            RUVIA_REQUIRED("健康恢复阈值不能为空"),
                            RUVIA_MIN(1, "健康恢复阈值必须在1到10之间"),
                            RUVIA_MAX(10, "健康恢复阈值必须在1到10之间")),
            RUVIA_RULE_NAME(
                "unhealthy_threshold", unhealthyThreshold, RUVIA_REQUIRED("故障阈值不能为空"),
                RUVIA_MIN(1, "故障阈值必须在1到10之间"), RUVIA_MAX(10, "故障阈值必须在1到10之间")),
            RUVIA_RULE_NAME("access_log_enabled", accessLogEnabled,
                            RUVIA_REQUIRED("访问日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_request_headers", accessLogRequestHeaders,
                            RUVIA_REQUIRED("请求头日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_request_body", accessLogRequestBody,
                            RUVIA_REQUIRED("请求体日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_response_headers", accessLogResponseHeaders,
                            RUVIA_REQUIRED("响应头日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_query_params", accessLogQueryParams,
                            RUVIA_REQUIRED("参数日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_cookies", accessLogCookies,
                            RUVIA_REQUIRED("Cookie 日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_referer", accessLogReferer,
                            RUVIA_REQUIRED("请求来源日志设置不能为空")),
            RUVIA_RULE_NAME("access_log_user_agent", accessLogUserAgent,
                            RUVIA_REQUIRED("终端信息日志设置不能为空")),
            RUVIA_RULE_NAME(
                "access_log_status_code_ranges", accessLogStatusCodeRanges,
                RUVIA_REQUIRED("请选择访问日志状态码"),
                RUVIA_CUSTOM("访问日志状态码范围不正确", validAccessLogStatusCodeRanges)),
            RUVIA_RULE_NAME("access_log_client_abort", accessLogClientAbort,
                            RUVIA_REQUIRED("客户端中断日志设置不能为空")),
            RUVIA_RULE_NAME("https_enabled", httpsEnabled, RUVIA_REQUIRED("HTTPS 设置不能为空")),
            RUVIA_RULE_NAME(
                "certificate_ids", certificateIds, RUVIA_REQUIRED("certificate_ids 不能为空"),
                RUVIA_MAX(kMaxWebsiteCertificates, "最多绑定20张证书"),
                RUVIA_CUSTOM("证书 ID 必须为 UUID 且不能重复", hasUniqueCertificateIds)),
            RUVIA_RULE_NAME("minimum_tls_version", minimumTlsVersion,
                            RUVIA_REQUIRED("请选择最低 TLS 版本"),
                            RUVIA_REGEX("最低 TLS 版本不正确", R"(^(1\.2|1\.3)$)")),
            RUVIA_RULE_NAME("force_https", forceHttps, RUVIA_REQUIRED("HTTPS 跳转设置不能为空")),
            RUVIA_RULE_NAME("http2_enabled", http2Enabled, RUVIA_REQUIRED("HTTP/2 设置不能为空")),
            RUVIA_RULE_NAME("hsts_enabled", hstsEnabled, RUVIA_REQUIRED("HSTS 设置不能为空")),
            RUVIA_RULE_NAME("response_compression_enabled", responseCompressionEnabled,
                            RUVIA_REQUIRED("响应压缩设置不能为空")),
            RUVIA_RULE_NAME("response_compression_min_bytes", responseCompressionMinBytes,
                            RUVIA_REQUIRED("响应压缩阈值不能为空"),
                            RUVIA_MIN(256, "响应压缩阈值必须在256字节到1 MiB之间"),
                            RUVIA_MAX(1048576, "响应压缩阈值必须在256字节到1 MiB之间")),
            RUVIA_RULE_NAME("response_compression_max_bytes", responseCompressionMaxBytes,
                            RUVIA_REQUIRED("响应压缩最大长度不能为空"),
                            RUVIA_MIN(0, "响应压缩最大长度不能小于0"),
                            RUVIA_MAX(67108864, "响应压缩最大长度不能超过64 MiB")),
            RUVIA_RULE_NAME("response_compression_algorithms", responseCompressionAlgorithms,
                            RUVIA_REQUIRED("压缩算法不能为空"),
                            RUVIA_CUSTOM("压缩算法必须从 br、zstd、gzip 中选择且不能重复",
                                         validCompressionAlgorithms)),
            RUVIA_RULE_NAME("response_compression_mime_types", responseCompressionMimeTypes,
                            RUVIA_REQUIRED("压缩 MIME 类型不能为空"),
                            RUVIA_CUSTOM("压缩 MIME 类型不正确", validCompressionMimeTypes)),
            RUVIA_RULE_NAME("response_compression_extensions", responseCompressionExtensions,
                            RUVIA_REQUIRED("压缩扩展名不能为空"),
                            RUVIA_CUSTOM("压缩扩展名不正确", validCompressionExtensions)),
            RUVIA_RULE_NAME("response_compression_excluded_extensions",
                            responseCompressionExcludedExtensions,
                            RUVIA_REQUIRED("例外扩展名不能为空"),
                            RUVIA_CUSTOM("例外扩展名不正确", validCompressionExtensions)),
            RUVIA_RULE_NAME("route_rules", routeRules, RUVIA_REQUIRED("路由规则不能为空"),
                            RUVIA_MAX(100, "最多配置100条路由规则"),
                            RUVIA_CUSTOM("路由规则 ID 和请求头名称必须唯一", validRouteRuleSet),
                            RUVIA_EACH(WebsiteRouteRuleValidator)))
        const auto& minimum = body.get<"responseCompressionMinBytes">();
        const auto& maximum = body.get<"responseCompressionMaxBytes">();
        if (minimum && maximum && maximum->value != 0 && maximum->value < minimum->value) {
            std::string path(prefix);
            if (!path.empty()) {
                path.push_back('.');
            }
            path.append("response_compression_max_bytes");
            validator.add(path, "range", "最大压缩大小不能小于最小压缩大小");
        }
        const auto& origins = body.get<"origins">();
        const auto& defaultOriginGroup = body.get<"defaultOriginGroup">();
        const auto& routeRules = body.get<"routeRules">();
        if (!origins || !defaultOriginGroup || !routeRules) {
            return;
        }
        std::unordered_set<std::string_view> enabledOriginGroups;
        for (const auto& origin : *origins) {
            const auto& group = origin.get<"group">();
            const auto& status = origin.get<"status">();
            if (group && status && status->view() == "enabled") {
                enabledOriginGroups.emplace(group->view());
            }
        }
        std::string defaultPath(prefix);
        if (!defaultPath.empty())
            defaultPath.push_back('.');
        defaultPath.append("default_origin_group");
        if (!enabledOriginGroups.contains(defaultOriginGroup->view())) {
            validator.add(defaultPath, "reference", "默认源站组必须包含启用的源站");
        }
        std::size_t routeIndex{};
        for (const auto& route : *routeRules) {
            const auto& action = route.get<"action">();
            const auto& originGroup = route.get<"originGroup">();
            if (!action || !originGroup) {
                ++routeIndex;
                continue;
            }
            std::string path(prefix);
            if (!path.empty()) {
                path.push_back('.');
            }
            path.append("route_rules[").append(std::to_string(routeIndex)).append("].origin_group");
            if (action->view() == "redirect" && !originGroup->empty()) {
                validator.add(path, "forbidden", "跳转规则不能选择源站组");
            }
            if (action->view() == "proxy" && !enabledOriginGroups.contains(originGroup->view())) {
                validator.add(path, "reference", "只能选择包含启用源站的源站组");
            }
            ++routeIndex;
        }
    }
};

class WebsiteConfigValidator final : public ruvia::Middleware<WebsiteConfigValidator> {
    RUVIA_VALIDATE_JSON(WebsiteSaveInput,
                        RUVIA_RULE(status, RUVIA_REQUIRED("网站状态不能为空"),
                                   RUVIA_REGEX("网站状态不正确", R"(^(enabled|disabled)$)")),
                        RUVIA_RULE(config, RUVIA_REQUIRED("网站配置不能为空"),
                                   RUVIA_NESTED(WebsiteConfigRules)))
};

} // namespace service::website
