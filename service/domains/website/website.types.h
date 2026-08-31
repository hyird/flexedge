#pragma once

#include <optional>
#include <string>
#include <utility>

#include <ruvia/web/Model.h>

#include "service/features/website_config/model.h"

namespace service::website {

RUVIA_REQUEST_MODEL(WebsiteSaveInput, RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(config, service::website_config::WebsiteConfigInput));

struct WebsiteSaveData final {
    std::string status;
    service::website_config::WebsiteConfigData config;
};

[[nodiscard]] inline std::optional<WebsiteSaveData> normalize(const WebsiteSaveInput& input) {
    const auto& status = input.get<"status">();
    const auto& config = input.get<"config">();
    if (!status || !config) {
        return std::nullopt;
    }
    auto normalizedConfig = service::website_config::normalize(*config);
    if (!normalizedConfig) {
        return std::nullopt;
    }
    return WebsiteSaveData{.status = std::string(status->view()),
                           .config = std::move(*normalizedConfig)};
}

RUVIA_RESPONSE_MODEL(
    WebsiteDomainRuntimeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("access_protocol", accessProtocol, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("resolution_status", resolutionStatus, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_verified_at", lastVerifiedAt, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(
    WebsiteOriginRuntimeDto, RUVIA_REQUIRED_FIELD_NAME("node_id", nodeId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("node_name", nodeName, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("origin_id", originId, ruvia::String),
    RUVIA_REQUIRED_FIELD(status, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("checked_at_unix_millis", checkedAtUnixMillis, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("latency_millis", latencyMillis, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_RESPONSE_MODEL(WebsiteRuntimeDto,
                     RUVIA_REQUIRED_FIELD_NAME("domain_states", domainStates,
                                               ruvia::Array<WebsiteDomainRuntimeDto>),
                     RUVIA_REQUIRED_FIELD_NAME("origin_states", originStates,
                                               ruvia::Array<WebsiteOriginRuntimeDto>),
                     RUVIA_REQUIRED_FIELD_NAME("deploy_status", deployStatus, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("target_node_count", targetNodeCount, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("synced_node_count", syncedNodeCount, ruvia::Int64));

RUVIA_RESPONSE_MODEL(WebsiteCertificateDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD(domains, ruvia::Array<ruvia::String>),
                     RUVIA_REQUIRED_FIELD(usable, ruvia::Bool));

RUVIA_RESPONSE_MODEL(WebsiteDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("cluster_id", clusterId, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("cluster_name", clusterName, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("access_domain", accessDomain, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(config, service::website_config::WebsiteConfigOutput),
                     RUVIA_REQUIRED_FIELD(certificates, ruvia::Array<WebsiteCertificateDto>),
                     RUVIA_REQUIRED_FIELD(runtime, WebsiteRuntimeDto),
                     RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(WebsitePageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<WebsiteDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_RESPONSE_MODEL(WebsitePageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, WebsitePageDataDto));
RUVIA_RESPONSE_MODEL(WebsiteDetailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, WebsiteDto));

RUVIA_RESPONSE_MODEL(
    WebsiteAccessLogDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("occurred_at", occurredAt, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("node_id", nodeId, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("node_name", nodeName, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("client_ip", clientIp, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD(protocol, ruvia::String), RUVIA_REQUIRED_FIELD(method, ruvia::String),
    RUVIA_REQUIRED_FIELD(host, ruvia::String), RUVIA_REQUIRED_FIELD(target, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("status_code", statusCode, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("response_bytes", responseBytes, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("duration_ms", durationMs, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("user_agent", userAgent, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(referer, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("request_headers", requestHeaders, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("request_body", requestBody, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("request_body_truncated", requestBodyTruncated, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD_NAME("tls_fingerprint", tlsFingerprint, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("response_headers", responseHeaders, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("query_string", queryString, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(cookies, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(WebsiteAccessLogTailDataDto,
                     RUVIA_REQUIRED_FIELD(list, ruvia::Array<WebsiteAccessLogDto>),
                     RUVIA_OPTIONAL_FIELD(cursor, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(WebsiteAccessLogTailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, WebsiteAccessLogTailDataDto));

} // namespace service::website
