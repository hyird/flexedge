#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "service/common/domain_name.h"
#include "service/features/node_config/model.h"
#include "service/features/website_config/model.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node_dispatch {

constexpr bool canReportAppliedNodeSpecRevision(std::int64_t storedAppliedRevision,
                                                std::int64_t reportedAppliedRevision,
                                                std::int64_t desiredRevision) noexcept {
    return storedAppliedRevision <= reportedAppliedRevision &&
           reportedAppliedRevision <= desiredRevision;
}

struct AvailableCertificate final {
    std::string id;
    std::string domain;
    std::string certificateChainPem;
    std::string privateKeyEnvelope;
};

struct DeploymentWebsiteSource final {
    std::string id;
    std::int64_t revision;
    bool enabled;
    std::string configJson;
};

struct ClusterDeploymentSource final {
    std::string clusterId;
    std::string tenantId;
    std::string accessDomain;
    bool enabled;
    std::vector<DeploymentWebsiteSource> websites;
    std::unordered_map<std::string, std::vector<AvailableCertificate>> certificatesByWebsite;
};

struct ClusterReleaseArtifact final {
    flexedge::node::v2::ClusterReleaseManifest manifest;
    std::vector<flexedge::node::v2::DeliveryObject> objects;
};

inline flexedge::node::v2::NodeSpec buildNodeSpec(std::string_view nodeId, std::int64_t revision,
                                                  std::string_view name, bool enabled,
                                                  std::string_view configJson) {
    const std::optional<service::node_config::NodeConfigData> config =
        service::node_config::parseStored(configJson);
    if (!config) {
        throw std::runtime_error("node spec source is invalid");
    }
    flexedge::node::v2::NodeSpec result;
    auto* content = result.mutable_content();
    content->set_node_id(nodeId);
    content->set_schema_version(flexedge::node::kNodeSpecSchemaVersion);
    content->set_revision(revision);
    content->set_name(name);
    content->set_enabled(enabled);
    for (const auto& input : config->endpoints) {
        auto* endpoint = content->add_endpoints();
        endpoint->set_id(input.id);
        endpoint->set_ip_address(input.ipAddress);
        endpoint->set_line_code(input.lineCode);
        endpoint->set_line_name(input.lineCode == "default" ? "默认" : input.lineCode);
        endpoint->set_http_port(80);
        endpoint->set_https_port(443);
    }
    result.set_digest_sha256(flexedge::node::artifactDigest(*content));
    return result;
}

inline flexedge::node::v2::DeliveryObject
deliveryObject(flexedge::node::v2::DeliveryObjectContent content) {
    flexedge::node::v2::DeliveryObject result;
    result.set_digest_sha256(flexedge::node::artifactDigest(content));
    *result.mutable_content() = std::move(content);
    return result;
}

template <typename Input, typename Output>
inline void copyStringValues(const Input& input, Output* output) {
    for (const auto& value : input) {
        *output->Add() = value;
    }
}

inline const std::string&
ensureCertificate(const AvailableCertificate& source,
                  std::unordered_map<std::string, std::string>& certificateDigests,
                  flexedge::node::v2::ClusterReleaseContent& manifest,
                  ClusterReleaseArtifact& result) {
    if (const auto found = certificateDigests.find(source.id); found != certificateDigests.end()) {
        return found->second;
    }
    service::utils::SensitiveString privateKey(
        service::utils::openSecret(source.privateKeyEnvelope));
    flexedge::node::v2::DeliveryObjectContent content;
    auto* certificate = content.mutable_certificate();
    certificate->set_id(source.id);
    certificate->set_certificate_chain_pem(source.certificateChainPem);
    certificate->set_private_key_pem(std::string(privateKey.view()));
    auto object = deliveryObject(std::move(content));
    const auto digest = object.digest_sha256();
    auto* reference = manifest.add_objects();
    reference->set_kind(flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
    reference->set_digest_sha256(digest);
    result.objects.push_back(std::move(object));
    return certificateDigests.emplace(source.id, digest).first->second;
}

inline bool appendWebsiteDomains(const DeploymentWebsiteSource& source,
                                 const service::website_config::WebsiteConfigData& config,
                                 const ClusterDeploymentSource& deployment,
                                 std::unordered_map<std::string, std::string>& certificateDigests,
                                 flexedge::node::v2::ClusterReleaseContent& manifest,
                                 ClusterReleaseArtifact& result,
                                 flexedge::node::v2::Website& website) {
    const auto available = deployment.certificatesByWebsite.find(source.id);
    const bool configuredHttps = config.httpsEnabled;
    bool effectiveHttps = false;
    for (const auto& input : config.domains) {
        auto* domain = website.add_domains();
        const auto& hostname = input.hostname;
        domain->set_hostname(hostname);
        if (configuredHttps && available != deployment.certificatesByWebsite.end()) {
            for (const auto& certificate : available->second) {
                if (!service::common::certificateCoversHostname(certificate.domain, hostname)) {
                    continue;
                }
                domain->set_https_enabled(true);
                domain->set_certificate_digest(
                    ensureCertificate(certificate, certificateDigests, manifest, result));
                effectiveHttps = true;
                break;
            }
        }
    }
    return effectiveHttps;
}

inline void appendWebsiteOrigins(const service::website_config::WebsiteConfigData& config,
                                 flexedge::node::v2::Website& website) {
    for (const auto& input : config.origins) {
        auto* origin = website.add_origins();
        origin->set_id(input.id);
        origin->set_group(input.group);
        origin->set_protocol(input.protocol);
        origin->set_host(input.host);
        origin->set_port(static_cast<std::uint32_t>(input.port));
        origin->set_role(input.role);
        origin->set_weight(static_cast<std::uint32_t>(input.weight));
        origin->set_enabled(input.status == "enabled");
    }
}

inline flexedge::node::v2::DeliveryObject
buildWebsiteObject(const DeploymentWebsiteSource& source, const ClusterDeploymentSource& deployment,
                   std::unordered_map<std::string, std::string>& certificateDigests,
                   flexedge::node::v2::ClusterReleaseContent& manifest,
                   ClusterReleaseArtifact& result) {
    const std::optional<service::website_config::WebsiteConfigData> config =
        service::website_config::parseStored(source.configJson);
    if (!config) {
        throw std::runtime_error("website release source is invalid");
    }
    flexedge::node::v2::DeliveryObjectContent objectContent;
    auto* website = objectContent.mutable_website();
    website->set_id(source.id);
    website->set_enabled(source.enabled);
    website->set_revision(source.revision);
    website->set_origin_host_header(config->originHostHeader);
    website->set_default_origin_group(config->defaultOriginGroup);
    website->set_origin_connect_timeout_seconds(
        static_cast<std::uint32_t>(config->originConnectTimeoutSeconds));
    website->set_origin_read_timeout_seconds(
        static_cast<std::uint32_t>(config->originReadTimeoutSeconds));
    website->set_pass_client_ip(config->passClientIp);
    website->set_health_check_enabled(config->healthCheckEnabled);
    website->set_access_log_enabled(config->accessLogEnabled);
    website->set_access_log_request_headers(config->accessLogRequestHeaders);
    website->set_access_log_request_body(config->accessLogRequestBody);
    website->set_access_log_response_headers(config->accessLogResponseHeaders);
    website->set_access_log_query_params(config->accessLogQueryParams);
    website->set_access_log_cookies(config->accessLogCookies);
    website->set_access_log_referer(config->accessLogReferer);
    website->set_access_log_user_agent(config->accessLogUserAgent);
    website->set_access_log_client_abort(config->accessLogClientAbort);
    copyStringValues(config->accessLogStatusCodeRanges,
                     website->mutable_access_log_status_code_ranges());
    website->set_minimum_tls_version(config->minimumTlsVersion);
    website->set_health_check_path(config->healthCheckPath);
    website->set_health_check_interval_seconds(
        static_cast<std::uint32_t>(config->healthCheckIntervalSeconds));
    website->set_health_check_timeout_seconds(
        static_cast<std::uint32_t>(config->healthCheckTimeoutSeconds));
    website->set_health_check_expected_status(
        static_cast<std::uint32_t>(config->healthCheckExpectedStatus));
    website->set_healthy_threshold(static_cast<std::uint32_t>(config->healthyThreshold));
    website->set_unhealthy_threshold(static_cast<std::uint32_t>(config->unhealthyThreshold));
    website->set_response_compression_enabled(config->responseCompressionEnabled);
    website->set_response_compression_min_bytes(
        static_cast<std::uint32_t>(config->responseCompressionMinBytes));
    website->set_response_compression_max_bytes(
        static_cast<std::uint32_t>(config->responseCompressionMaxBytes));
    copyStringValues(config->responseCompressionAlgorithms,
                     website->mutable_response_compression_algorithms());
    copyStringValues(config->responseCompressionMimeTypes,
                     website->mutable_response_compression_mime_types());
    copyStringValues(config->responseCompressionExtensions,
                     website->mutable_response_compression_extensions());
    copyStringValues(config->responseCompressionExcludedExtensions,
                     website->mutable_response_compression_excluded_extensions());
    for (const auto& input : config->routeRules) {
        auto* rule = website->add_route_rules();
        rule->set_id(input.id);
        rule->set_enabled(input.status == "enabled");
        rule->set_match_type(input.matchType);
        rule->set_path(input.path);
        copyStringValues(input.methods, rule->mutable_methods());
        rule->set_action(input.action);
        rule->set_rewrite_path(input.rewritePath);
        rule->set_redirect_url(input.redirectUrl);
        rule->set_redirect_status(static_cast<std::uint32_t>(input.redirectStatus));
        rule->set_origin_group(input.originGroup);
        for (const auto& header : input.requestHeaders) {
            auto* target = rule->add_request_headers();
            target->set_name(header.name);
            target->set_value(header.value);
        }
        for (const auto& header : input.responseHeaders) {
            auto* target = rule->add_response_headers();
            target->set_name(header.name);
            target->set_value(header.value);
        }
    }

    const bool effectiveHttps = appendWebsiteDomains(
        source, *config, deployment, certificateDigests, manifest, result, *website);
    website->set_https_enabled(effectiveHttps);
    website->set_force_https(effectiveHttps && config->forceHttps);
    website->set_http2_enabled(effectiveHttps && config->http2Enabled);
    website->set_hsts_enabled(effectiveHttps && config->hstsEnabled);
    appendWebsiteOrigins(*config, *website);
    return deliveryObject(std::move(objectContent));
}

inline ClusterReleaseArtifact buildClusterRelease(std::string_view releaseId,
                                                  std::int64_t generation,
                                                  const ClusterDeploymentSource& deployment) {
    ClusterReleaseArtifact result;
    auto* manifest = result.manifest.mutable_content();
    manifest->set_cluster_id(deployment.clusterId);
    manifest->set_release_id(releaseId);
    manifest->set_generation(generation);
    manifest->set_access_domain(deployment.accessDomain);
    manifest->set_enabled(deployment.enabled);
    manifest->set_schema_version(flexedge::node::kClusterReleaseSchemaVersion);

    std::unordered_map<std::string, std::string> certificateDigests;
    for (const auto& source : deployment.websites) {
        auto object = buildWebsiteObject(source, deployment, certificateDigests, *manifest, result);
        auto* reference = manifest->add_objects();
        reference->set_kind(flexedge::node::v2::OBJECT_KIND_WEBSITE);
        reference->set_digest_sha256(object.digest_sha256());
        result.objects.push_back(std::move(object));
    }
    result.manifest.set_digest_sha256(flexedge::node::artifactDigest(*manifest));
    return result;
}

} // namespace service::node_dispatch
