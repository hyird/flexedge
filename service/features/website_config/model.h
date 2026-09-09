#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::website_config {

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

struct WebsiteRouteConditionData final {
    std::string source;
    std::string name;
    std::string op;
    std::string value;
};

struct WebsiteRouteRuleData final {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> hostnames;
    std::vector<WebsiteRouteConditionData> conditions;
    std::string rewriteMode;
    std::string queryMode;
    std::string queryString;
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

} // namespace service::website_config
