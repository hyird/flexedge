#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::website_config {

RUVIA_REQUEST_MODEL(WebsiteDomainInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(hostname, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("dns_mode", dnsMode, ruvia::String));
RUVIA_REQUEST_MODEL(WebsiteOriginInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("group", group, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(protocol, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(host, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(port, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(role, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(weight, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String));
RUVIA_REQUEST_MODEL(WebsiteRouteHeaderInput, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(value, ruvia::String));
RUVIA_REQUEST_MODEL(WebsiteRouteRuleInput, RUVIA_OPTIONAL_FIELD(id, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("match_type", matchType, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(path, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(methods, ruvia::Array<ruvia::String>),
                    RUVIA_OPTIONAL_FIELD(action, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("rewrite_path", rewritePath, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("redirect_url", redirectUrl, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("redirect_status", redirectStatus, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("origin_group", originGroup, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("request_headers", requestHeaders,
                                              ruvia::Array<WebsiteRouteHeaderInput>),
                    RUVIA_OPTIONAL_FIELD_NAME("response_headers", responseHeaders,
                                              ruvia::Array<WebsiteRouteHeaderInput>));
RUVIA_REQUEST_MODEL(
    WebsiteConfigInput, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(domains, ruvia::Array<WebsiteDomainInput>),
    RUVIA_OPTIONAL_FIELD(origins, ruvia::Array<WebsiteOriginInput>),
    RUVIA_OPTIONAL_FIELD_NAME("default_origin_group", defaultOriginGroup, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("origin_host_header", originHostHeader, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("origin_connect_timeout_seconds", originConnectTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("origin_read_timeout_seconds", originReadTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("pass_client_ip", passClientIp, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("health_check_enabled", healthCheckEnabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("health_check_path", healthCheckPath, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("health_check_interval_seconds", healthCheckIntervalSeconds,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("health_check_timeout_seconds", healthCheckTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("health_check_expected_status", healthCheckExpectedStatus,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("healthy_threshold", healthyThreshold, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("unhealthy_threshold", unhealthyThreshold, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_enabled", accessLogEnabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_request_headers", accessLogRequestHeaders, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_request_body", accessLogRequestBody, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_response_headers", accessLogResponseHeaders, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_query_params", accessLogQueryParams, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_cookies", accessLogCookies, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_referer", accessLogReferer, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_user_agent", accessLogUserAgent, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_status_code_ranges", accessLogStatusCodeRanges,
                              ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("access_log_client_abort", accessLogClientAbort, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("https_enabled", httpsEnabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("certificate_ids", certificateIds, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("minimum_tls_version", minimumTlsVersion, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("force_https", forceHttps, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("http2_enabled", http2Enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("hsts_enabled", hstsEnabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_enabled", responseCompressionEnabled,
                              ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_min_bytes", responseCompressionMinBytes,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_max_bytes", responseCompressionMaxBytes,
                              ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_algorithms", responseCompressionAlgorithms,
                              ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_mime_types", responseCompressionMimeTypes,
                              ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_extensions", responseCompressionExtensions,
                              ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("response_compression_excluded_extensions",
                              responseCompressionExcludedExtensions, ruvia::Array<ruvia::String>),
    RUVIA_OPTIONAL_FIELD_NAME("route_rules", routeRules, ruvia::Array<WebsiteRouteRuleInput>));

RUVIA_RESPONSE_MODEL(WebsiteDomainOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(hostname, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("dns_mode", dnsMode, ruvia::String));
RUVIA_RESPONSE_MODEL(WebsiteOriginOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("group", group, ruvia::String),
                     RUVIA_REQUIRED_FIELD(protocol, ruvia::String),
                     RUVIA_REQUIRED_FIELD(host, ruvia::String),
                     RUVIA_REQUIRED_FIELD(port, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(role, ruvia::String),
                     RUVIA_REQUIRED_FIELD(weight, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String));
RUVIA_RESPONSE_MODEL(WebsiteRouteHeaderOutput, RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(value, ruvia::String));
RUVIA_RESPONSE_MODEL(WebsiteRouteRuleOutput, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("match_type", matchType, ruvia::String),
                     RUVIA_REQUIRED_FIELD(path, ruvia::String),
                     RUVIA_REQUIRED_FIELD(methods, ruvia::Array<ruvia::String>),
                     RUVIA_REQUIRED_FIELD(action, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("rewrite_path", rewritePath, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("redirect_url", redirectUrl, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("redirect_status", redirectStatus, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("origin_group", originGroup, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("request_headers", requestHeaders,
                                               ruvia::Array<WebsiteRouteHeaderOutput>),
                     RUVIA_REQUIRED_FIELD_NAME("response_headers", responseHeaders,
                                               ruvia::Array<WebsiteRouteHeaderOutput>));
RUVIA_RESPONSE_MODEL(
    WebsiteConfigOutput, RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_REQUIRED_FIELD(domains, ruvia::Array<WebsiteDomainOutput>),
    RUVIA_REQUIRED_FIELD(origins, ruvia::Array<WebsiteOriginOutput>),
    RUVIA_REQUIRED_FIELD_NAME("default_origin_group", defaultOriginGroup, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_host_header", originHostHeader, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_connect_timeout_seconds", originConnectTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("origin_read_timeout_seconds", originReadTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("pass_client_ip", passClientIp, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("health_check_enabled", healthCheckEnabled, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("health_check_path", healthCheckPath, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("health_check_interval_seconds", healthCheckIntervalSeconds,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("health_check_timeout_seconds", healthCheckTimeoutSeconds,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("health_check_expected_status", healthCheckExpectedStatus,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("healthy_threshold", healthyThreshold, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("unhealthy_threshold", unhealthyThreshold, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("access_log_enabled", accessLogEnabled, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_request_headers", accessLogRequestHeaders, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_request_body", accessLogRequestBody, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_response_headers", accessLogResponseHeaders, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_query_params", accessLogQueryParams, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_cookies", accessLogCookies, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_referer", accessLogReferer, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_user_agent", accessLogUserAgent, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("access_log_status_code_ranges", accessLogStatusCodeRanges,
                              ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("access_log_client_abort", accessLogClientAbort, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("https_enabled", httpsEnabled, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("certificate_ids", certificateIds, ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("minimum_tls_version", minimumTlsVersion, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("force_https", forceHttps, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("http2_enabled", http2Enabled, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("hsts_enabled", hstsEnabled, ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_enabled", responseCompressionEnabled,
                              ruvia::Bool),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_min_bytes", responseCompressionMinBytes,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_max_bytes", responseCompressionMaxBytes,
                              ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_algorithms", responseCompressionAlgorithms,
                              ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_mime_types", responseCompressionMimeTypes,
                              ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_extensions", responseCompressionExtensions,
                              ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("response_compression_excluded_extensions",
                              responseCompressionExcludedExtensions, ruvia::Array<ruvia::String>),
    RUVIA_REQUIRED_FIELD_NAME("route_rules", routeRules, ruvia::Array<WebsiteRouteRuleOutput>));

struct WebsiteDomainData final {
    std::string id;
    std::string hostname;
    std::string dnsMode;
};

struct WebsiteOriginData final {
    std::string id;
    std::string group;
    std::string protocol;
    std::string host;
    std::int64_t port;
    std::string role;
    std::int64_t weight;
    std::string status;
};

struct WebsiteRouteHeaderData final {
    std::string name;
    std::string value;
};

struct WebsiteRouteRuleData final {
    std::string id;
    std::string status;
    std::string matchType;
    std::string path;
    std::vector<std::string> methods;
    std::string action;
    std::string rewritePath;
    std::string redirectUrl;
    std::int64_t redirectStatus;
    std::string originGroup;
    std::vector<WebsiteRouteHeaderData> requestHeaders;
    std::vector<WebsiteRouteHeaderData> responseHeaders;
};

struct WebsiteConfigData final {
    std::optional<std::string> name;
    std::vector<WebsiteDomainData> domains;
    std::vector<WebsiteOriginData> origins;
    std::string defaultOriginGroup;
    std::string originHostHeader;
    std::int64_t originConnectTimeoutSeconds;
    std::int64_t originReadTimeoutSeconds;
    bool passClientIp;
    bool healthCheckEnabled;
    std::string healthCheckPath;
    std::int64_t healthCheckIntervalSeconds;
    std::int64_t healthCheckTimeoutSeconds;
    std::int64_t healthCheckExpectedStatus;
    std::int64_t healthyThreshold;
    std::int64_t unhealthyThreshold;
    bool accessLogEnabled;
    bool accessLogRequestHeaders;
    bool accessLogRequestBody;
    bool accessLogResponseHeaders;
    bool accessLogQueryParams;
    bool accessLogCookies;
    bool accessLogReferer;
    bool accessLogUserAgent;
    std::vector<std::string> accessLogStatusCodeRanges;
    bool accessLogClientAbort;
    bool httpsEnabled;
    std::vector<std::string> certificateIds;
    std::string minimumTlsVersion;
    bool forceHttps;
    bool http2Enabled;
    bool hstsEnabled;
    bool responseCompressionEnabled;
    std::int64_t responseCompressionMinBytes;
    std::int64_t responseCompressionMaxBytes;
    std::vector<std::string> responseCompressionAlgorithms;
    std::vector<std::string> responseCompressionMimeTypes;
    std::vector<std::string> responseCompressionExtensions;
    std::vector<std::string> responseCompressionExcludedExtensions;
    std::vector<WebsiteRouteRuleData> routeRules;
};

[[nodiscard]] inline std::optional<WebsiteDomainData> normalize(const WebsiteDomainInput& input) {
    const auto& id = input.get<"id">();
    const auto& hostname = input.get<"hostname">();
    const auto& dnsMode = input.get<"dnsMode">();
    if (!id || !hostname || !dnsMode) {
        return std::nullopt;
    }
    return WebsiteDomainData{.id = std::string(id->view()),
                             .hostname = std::string(hostname->view()),
                             .dnsMode = std::string(dnsMode->view())};
}

[[nodiscard]] inline std::optional<WebsiteOriginData> normalize(const WebsiteOriginInput& input) {
    const auto& id = input.get<"id">();
    const auto& group = input.get<"group">();
    const auto& protocol = input.get<"protocol">();
    const auto& host = input.get<"host">();
    const auto& port = input.get<"port">();
    const auto& role = input.get<"role">();
    const auto& weight = input.get<"weight">();
    const auto& status = input.get<"status">();
    if (!id || !protocol || !host || !port || !role || !weight || !status) {
        return std::nullopt;
    }
    return WebsiteOriginData{.id = std::string(id->view()),
                             .group = group ? std::string(group->view()) : "default",
                             .protocol = std::string(protocol->view()),
                             .host = std::string(host->view()),
                             .port = port->value,
                             .role = std::string(role->view()),
                             .weight = weight->value,
                             .status = std::string(status->view())};
}

[[nodiscard]] inline std::optional<WebsiteRouteHeaderData>
normalize(const WebsiteRouteHeaderInput& input) {
    const auto& name = input.get<"name">();
    const auto& value = input.get<"value">();
    if (!name || !value) {
        return std::nullopt;
    }
    return WebsiteRouteHeaderData{.name = std::string(name->view()),
                                  .value = std::string(value->view())};
}

[[nodiscard]] inline std::optional<WebsiteRouteRuleData>
normalize(const WebsiteRouteRuleInput& input) {
    const auto& id = input.get<"id">();
    const auto& status = input.get<"status">();
    const auto& matchType = input.get<"matchType">();
    const auto& path = input.get<"path">();
    const auto& methods = input.get<"methods">();
    const auto& action = input.get<"action">();
    const auto& rewritePath = input.get<"rewritePath">();
    const auto& redirectUrl = input.get<"redirectUrl">();
    const auto& redirectStatus = input.get<"redirectStatus">();
    const auto& originGroup = input.get<"originGroup">();
    const auto& requestHeaders = input.get<"requestHeaders">();
    const auto& responseHeaders = input.get<"responseHeaders">();
    if (!id || !status || !matchType || !path || !methods || !action || !rewritePath ||
        !redirectUrl || !redirectStatus || !requestHeaders || !responseHeaders) {
        return std::nullopt;
    }
    WebsiteRouteRuleData result{.id = std::string(id->view()),
                                .status = std::string(status->view()),
                                .matchType = std::string(matchType->view()),
                                .path = std::string(path->view()),
                                .methods = {},
                                .action = std::string(action->view()),
                                .rewritePath = std::string(rewritePath->view()),
                                .redirectUrl = std::string(redirectUrl->view()),
                                .redirectStatus = redirectStatus->value,
                                .originGroup = originGroup ? std::string(originGroup->view()) : "",
                                .requestHeaders = {},
                                .responseHeaders = {}};
    result.methods.reserve(methods->size());
    for (const auto& method : *methods) {
        result.methods.emplace_back(method.view());
    }
    result.requestHeaders.reserve(requestHeaders->size());
    for (const auto& header : *requestHeaders) {
        auto normalized = normalize(header);
        if (!normalized) {
            return std::nullopt;
        }
        result.requestHeaders.push_back(std::move(*normalized));
    }
    result.responseHeaders.reserve(responseHeaders->size());
    for (const auto& header : *responseHeaders) {
        auto normalized = normalize(header);
        if (!normalized) {
            return std::nullopt;
        }
        result.responseHeaders.push_back(std::move(*normalized));
    }
    return result;
}

namespace detail {

template <typename Values>
[[nodiscard]] inline std::vector<std::string> copyStrings(const Values& values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.emplace_back(value.view());
    }
    return result;
}

} // namespace detail

[[nodiscard]] inline std::optional<WebsiteConfigData> normalize(const WebsiteConfigInput& input) {
    const auto& domains = input.get<"domains">();
    const auto& origins = input.get<"origins">();
    const auto& defaultOriginGroup = input.get<"defaultOriginGroup">();
    const auto& originHostHeader = input.get<"originHostHeader">();
    const auto& originConnectTimeoutSeconds = input.get<"originConnectTimeoutSeconds">();
    const auto& originReadTimeoutSeconds = input.get<"originReadTimeoutSeconds">();
    const auto& passClientIp = input.get<"passClientIp">();
    const auto& healthCheckEnabled = input.get<"healthCheckEnabled">();
    const auto& healthCheckPath = input.get<"healthCheckPath">();
    const auto& healthCheckIntervalSeconds = input.get<"healthCheckIntervalSeconds">();
    const auto& healthCheckTimeoutSeconds = input.get<"healthCheckTimeoutSeconds">();
    const auto& healthCheckExpectedStatus = input.get<"healthCheckExpectedStatus">();
    const auto& healthyThreshold = input.get<"healthyThreshold">();
    const auto& unhealthyThreshold = input.get<"unhealthyThreshold">();
    const auto& accessLogEnabled = input.get<"accessLogEnabled">();
    const auto& accessLogRequestHeaders = input.get<"accessLogRequestHeaders">();
    const auto& accessLogRequestBody = input.get<"accessLogRequestBody">();
    const auto& accessLogResponseHeaders = input.get<"accessLogResponseHeaders">();
    const auto& accessLogQueryParams = input.get<"accessLogQueryParams">();
    const auto& accessLogCookies = input.get<"accessLogCookies">();
    const auto& accessLogReferer = input.get<"accessLogReferer">();
    const auto& accessLogUserAgent = input.get<"accessLogUserAgent">();
    const auto& accessLogStatusCodeRanges = input.get<"accessLogStatusCodeRanges">();
    const auto& accessLogClientAbort = input.get<"accessLogClientAbort">();
    const auto& httpsEnabled = input.get<"httpsEnabled">();
    const auto& certificateIds = input.get<"certificateIds">();
    const auto& minimumTlsVersion = input.get<"minimumTlsVersion">();
    const auto& forceHttps = input.get<"forceHttps">();
    const auto& http2Enabled = input.get<"http2Enabled">();
    const auto& hstsEnabled = input.get<"hstsEnabled">();
    const auto& responseCompressionEnabled = input.get<"responseCompressionEnabled">();
    const auto& responseCompressionMinBytes = input.get<"responseCompressionMinBytes">();
    const auto& responseCompressionMaxBytes = input.get<"responseCompressionMaxBytes">();
    const auto& responseCompressionAlgorithms = input.get<"responseCompressionAlgorithms">();
    const auto& responseCompressionMimeTypes = input.get<"responseCompressionMimeTypes">();
    const auto& responseCompressionExtensions = input.get<"responseCompressionExtensions">();
    const auto& responseCompressionExcludedExtensions =
        input.get<"responseCompressionExcludedExtensions">();
    const auto& routeRules = input.get<"routeRules">();
    if (!domains || !origins || !originHostHeader || !originConnectTimeoutSeconds ||
        !originReadTimeoutSeconds || !passClientIp || !healthCheckEnabled || !accessLogEnabled ||
        !accessLogRequestHeaders || !accessLogRequestBody || !accessLogResponseHeaders ||
        !accessLogQueryParams || !accessLogCookies || !accessLogReferer || !accessLogUserAgent ||
        !accessLogStatusCodeRanges || !accessLogClientAbort || !httpsEnabled || !certificateIds ||
        !minimumTlsVersion || !forceHttps || !http2Enabled || !hstsEnabled ||
        !responseCompressionEnabled || !responseCompressionMinBytes ||
        !responseCompressionMaxBytes || !responseCompressionAlgorithms ||
        !responseCompressionMimeTypes || !responseCompressionExtensions ||
        !responseCompressionExcludedExtensions || !routeRules) {
        return std::nullopt;
    }

    WebsiteConfigData result;
    if (const auto& name = input.get<"name">()) {
        result.name = std::string(name->view());
    }
    result.originHostHeader = std::string(originHostHeader->view());
    result.defaultOriginGroup =
        defaultOriginGroup ? std::string(defaultOriginGroup->view()) : "default";
    result.originConnectTimeoutSeconds = originConnectTimeoutSeconds->value;
    result.originReadTimeoutSeconds = originReadTimeoutSeconds->value;
    result.passClientIp = passClientIp->value;
    result.healthCheckEnabled = healthCheckEnabled->value;
    result.healthCheckPath = healthCheckPath ? std::string(healthCheckPath->view()) : "/";
    result.healthCheckIntervalSeconds =
        healthCheckIntervalSeconds ? healthCheckIntervalSeconds->value : 10;
    result.healthCheckTimeoutSeconds =
        healthCheckTimeoutSeconds ? healthCheckTimeoutSeconds->value : 3;
    result.healthCheckExpectedStatus =
        healthCheckExpectedStatus ? healthCheckExpectedStatus->value : 200;
    result.healthyThreshold = healthyThreshold ? healthyThreshold->value : 2;
    result.unhealthyThreshold = unhealthyThreshold ? unhealthyThreshold->value : 3;
    result.accessLogEnabled = accessLogEnabled->value;
    result.accessLogRequestHeaders = accessLogRequestHeaders->value;
    result.accessLogRequestBody = accessLogRequestBody->value;
    result.accessLogResponseHeaders = accessLogResponseHeaders->value;
    result.accessLogQueryParams = accessLogQueryParams->value;
    result.accessLogCookies = accessLogCookies->value;
    result.accessLogReferer = accessLogReferer->value;
    result.accessLogUserAgent = accessLogUserAgent->value;
    result.accessLogStatusCodeRanges = detail::copyStrings(*accessLogStatusCodeRanges);
    result.accessLogClientAbort = accessLogClientAbort->value;
    result.httpsEnabled = httpsEnabled->value;
    result.certificateIds = detail::copyStrings(*certificateIds);
    result.minimumTlsVersion = std::string(minimumTlsVersion->view());
    result.forceHttps = forceHttps->value;
    result.http2Enabled = http2Enabled->value;
    result.hstsEnabled = hstsEnabled->value;
    result.responseCompressionEnabled = responseCompressionEnabled->value;
    result.responseCompressionMinBytes = responseCompressionMinBytes->value;
    result.responseCompressionMaxBytes = responseCompressionMaxBytes->value;
    result.responseCompressionAlgorithms = detail::copyStrings(*responseCompressionAlgorithms);
    result.responseCompressionMimeTypes = detail::copyStrings(*responseCompressionMimeTypes);
    result.responseCompressionExtensions = detail::copyStrings(*responseCompressionExtensions);
    result.responseCompressionExcludedExtensions =
        detail::copyStrings(*responseCompressionExcludedExtensions);

    result.domains.reserve(domains->size());
    for (const auto& domain : *domains) {
        auto normalized = normalize(domain);
        if (!normalized) {
            return std::nullopt;
        }
        result.domains.push_back(std::move(*normalized));
    }
    result.origins.reserve(origins->size());
    for (const auto& origin : *origins) {
        auto normalized = normalize(origin);
        if (!normalized) {
            return std::nullopt;
        }
        result.origins.push_back(std::move(*normalized));
    }
    const bool legacyRouteRules = !defaultOriginGroup;
    if (!legacyRouteRules) {
        result.routeRules.reserve(routeRules->size());
        for (const auto& rule : *routeRules) {
            auto normalized = normalize(rule);
            if (!normalized) {
                return std::nullopt;
            }
            result.routeRules.push_back(std::move(*normalized));
        }
    }
    std::unordered_set<std::string_view> enabledOriginGroups;
    for (const auto& origin : result.origins) {
        if (origin.status == "enabled") {
            enabledOriginGroups.emplace(origin.group);
        }
    }
    if (!enabledOriginGroups.contains(result.defaultOriginGroup)) {
        return std::nullopt;
    }
    for (const auto& rule : result.routeRules) {
        if (rule.action == "redirect" && !rule.originGroup.empty()) {
            return std::nullopt;
        }
        if (rule.action == "proxy" && !enabledOriginGroups.contains(rule.originGroup)) {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] inline bool complete(const WebsiteDomainInput& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const WebsiteOriginInput& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline bool complete(const WebsiteConfigInput& input) {
    return normalize(input).has_value();
}

[[nodiscard]] inline std::optional<WebsiteConfigData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<WebsiteConfigInput> input =
        ruvia::fromJson<WebsiteConfigInput>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline WebsiteConfigOutput toOutput(const WebsiteConfigData& input,
                                                  ruvia::ModelOptions options = {}) {
    WebsiteConfigOutput output(options);
    if (input.name) {
        output.set<"name">(*input.name);
    }
    output.set<"originHostHeader">(input.originHostHeader);
    output.set<"defaultOriginGroup">(input.defaultOriginGroup);
    output.set<"originConnectTimeoutSeconds">(input.originConnectTimeoutSeconds);
    output.set<"originReadTimeoutSeconds">(input.originReadTimeoutSeconds);
    output.set<"passClientIp">(input.passClientIp);
    output.set<"healthCheckEnabled">(input.healthCheckEnabled);
    output.set<"healthCheckPath">(input.healthCheckPath);
    output.set<"healthCheckIntervalSeconds">(input.healthCheckIntervalSeconds);
    output.set<"healthCheckTimeoutSeconds">(input.healthCheckTimeoutSeconds);
    output.set<"healthCheckExpectedStatus">(input.healthCheckExpectedStatus);
    output.set<"healthyThreshold">(input.healthyThreshold);
    output.set<"unhealthyThreshold">(input.unhealthyThreshold);
    output.set<"accessLogEnabled">(input.accessLogEnabled);
    output.set<"accessLogRequestHeaders">(input.accessLogRequestHeaders);
    output.set<"accessLogRequestBody">(input.accessLogRequestBody);
    output.set<"accessLogResponseHeaders">(input.accessLogResponseHeaders);
    output.set<"accessLogQueryParams">(input.accessLogQueryParams);
    output.set<"accessLogCookies">(input.accessLogCookies);
    output.set<"accessLogReferer">(input.accessLogReferer);
    output.set<"accessLogUserAgent">(input.accessLogUserAgent);
    output.set<"accessLogClientAbort">(input.accessLogClientAbort);
    output.set<"httpsEnabled">(input.httpsEnabled);
    output.set<"minimumTlsVersion">(input.minimumTlsVersion);
    output.set<"forceHttps">(input.forceHttps);
    output.set<"http2Enabled">(input.http2Enabled);
    output.set<"hstsEnabled">(input.hstsEnabled);
    output.set<"responseCompressionEnabled">(input.responseCompressionEnabled);
    output.set<"responseCompressionMinBytes">(input.responseCompressionMinBytes);
    output.set<"responseCompressionMaxBytes">(input.responseCompressionMaxBytes);
    const auto copyStrings = [&](auto& target, const std::vector<std::string>& values) {
        target.reserve(values.size());
        for (const auto& value : values) {
            target.emplace_back(value, options);
        }
    };
    copyStrings(output.ensure<"responseCompressionAlgorithms">(),
                input.responseCompressionAlgorithms);
    copyStrings(output.ensure<"responseCompressionMimeTypes">(),
                input.responseCompressionMimeTypes);
    copyStrings(output.ensure<"responseCompressionExtensions">(),
                input.responseCompressionExtensions);
    copyStrings(output.ensure<"responseCompressionExcludedExtensions">(),
                input.responseCompressionExcludedExtensions);
    copyStrings(output.ensure<"accessLogStatusCodeRanges">(), input.accessLogStatusCodeRanges);
    auto& domains = output.ensure<"domains">();
    domains.reserve(input.domains.size());
    for (const auto& domain : input.domains) {
        auto& item = domains.emplace_back(options);
        item.set<"id">(domain.id);
        item.set<"hostname">(domain.hostname);
        item.set<"dnsMode">(domain.dnsMode);
    }
    auto& origins = output.ensure<"origins">();
    origins.reserve(input.origins.size());
    for (const auto& origin : input.origins) {
        auto& item = origins.emplace_back(options);
        item.set<"id">(origin.id);
        item.set<"group">(origin.group);
        item.set<"protocol">(origin.protocol);
        item.set<"host">(origin.host);
        item.set<"port">(origin.port);
        item.set<"role">(origin.role);
        item.set<"weight">(origin.weight);
        item.set<"status">(origin.status);
    }
    auto& certificates = output.ensure<"certificateIds">();
    certificates.reserve(input.certificateIds.size());
    for (const auto& id : input.certificateIds) {
        certificates.emplace_back(id, options);
    }
    auto copyHeaders = [&](auto& target, const std::vector<WebsiteRouteHeaderData>& headers) {
        target.reserve(headers.size());
        for (const auto& header : headers) {
            auto& item = target.emplace_back(options);
            item.template set<"name">(header.name);
            item.template set<"value">(header.value);
        }
    };
    auto& routeRules = output.ensure<"routeRules">();
    routeRules.reserve(input.routeRules.size());
    for (const auto& rule : input.routeRules) {
        auto& item = routeRules.emplace_back(options);
        item.set<"id">(rule.id);
        item.set<"status">(rule.status);
        item.set<"matchType">(rule.matchType);
        item.set<"path">(rule.path);
        item.set<"action">(rule.action);
        item.set<"rewritePath">(rule.rewritePath);
        item.set<"redirectUrl">(rule.redirectUrl);
        item.set<"redirectStatus">(rule.redirectStatus);
        copyStrings(item.ensure<"methods">(), rule.methods);
        item.set<"originGroup">(rule.originGroup);
        copyHeaders(item.ensure<"requestHeaders">(), rule.requestHeaders);
        copyHeaders(item.ensure<"responseHeaders">(), rule.responseHeaders);
    }
    return output;
}

} // namespace service::website_config
