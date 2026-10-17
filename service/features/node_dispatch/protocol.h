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
#include "node/proto/website_validation.h"
#include "service/common/domain_name.h"
#include "service/common/http.h"
#include "service/features/node_config/mapper.h"
#include "service/features/node_dispatch/deployment_source.h"
#include "service/features/node_dispatch/dispatch_error.h"
#include "service/features/website_config/mapper.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::node_dispatch {

struct cluster_release_artifact final {
    flexedge::node::v2::ClusterReleaseManifest manifest_;
    std::vector<flexedge::node::v2::DeliveryObject> objects_;
};

inline flexedge::node::v2::NodeSpec build_node_spec(std::string_view node_id, std::int64_t revision,
    std::string_view name, bool enabled,
    std::string_view config_json) {
    const std::optional<service::node_config::node_config_data> config =
        service::node_config::parse_stored(config_json);
    if (!config) {
        throw std::runtime_error("node spec source is invalid");
    }
    flexedge::node::v2::NodeSpecContent content;
    content.set_node_id(node_id);
    content.set_schema_version(flexedge::node::node_spec_schema_version);
    content.set_revision(revision);
    content.set_name(name);
    content.set_enabled(enabled);
    for (const auto& input : config->endpoints_) {
        auto* endpoint = content.add_endpoints();
        endpoint->set_id(input.id_);
        endpoint->set_ip_address(input.ip_address_);
        endpoint->set_line_code(input.line_code_);
        endpoint->set_line_name(input.line_code_ == "default" ? "默认" : input.line_code_);
        endpoint->set_http_port(80);
        endpoint->set_https_port(443);
    }
    return flexedge::node::pack_artifact<flexedge::node::v2::NodeSpec>(content);
}

inline flexedge::node::v2::DeliveryObject
delivery_object(flexedge::node::v2::DeliveryObjectContent content) {
    return flexedge::node::pack_artifact<flexedge::node::v2::DeliveryObject>(content);
}

template <typename input_type, typename output_type>
inline void copy_string_values(const input_type& input, output_type* output) {
    for (const auto& value : input) {
        *output->Add() = value;
    }
}

inline const std::string&
ensure_certificate(const available_certificate& source,
    std::unordered_map<std::string, std::string>& certificate_digests,
    flexedge::node::v2::ClusterReleaseContent& manifest,
    cluster_release_artifact& result) {
    if (const auto found = certificate_digests.find(source.id_); found != certificate_digests.end()) {
        return found->second;
    }
    service::utils::sensitive_string private_key(
        service::utils::open_secret(source.private_key_envelope_));
    flexedge::node::v2::DeliveryObjectContent content;
    auto* certificate = content.mutable_certificate();
    certificate->set_id(source.id_);
    certificate->set_certificate_chain_pem(source.certificate_chain_pem_);
    certificate->set_private_key_pem(std::string(private_key.view()));
    auto object = delivery_object(std::move(content));
    const auto digest = object.digest_sha256();
    auto* reference = manifest.add_objects();
    reference->set_kind(flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
    reference->set_digest_sha256(digest);
    result.objects_.push_back(std::move(object));
    return certificate_digests.emplace(source.id_, digest).first->second;
}

inline bool append_website_domains(const deployment_website_source& source,
    const service::website_config::website_config_data& config,
    const cluster_deployment_source& deployment,
    std::unordered_map<std::string, std::string>& certificate_digests,
    flexedge::node::v2::ClusterReleaseContent& manifest,
    cluster_release_artifact& result,
    flexedge::node::v2::Website& website) {
    const auto available = deployment.certificates_by_website_.find(source.id_);
    const bool configured_https = config.https_enabled_;
    bool effective_https = false;
    for (const auto& input : config.domains_) {
        auto* domain = website.add_domains();
        const auto& hostname = input.hostname_;
        domain->set_hostname(hostname);
        if (configured_https && available != deployment.certificates_by_website_.end()) {
            for (const auto& certificate : available->second) {
                if (!std::ranges::any_of(certificate.domains_, [&](const auto& name) {
                        return service::common::certificate_covers_hostname(name, hostname);
                    })) {
                    continue;
                }
                domain->set_https_enabled(true);
                domain->set_certificate_digest(
                    ensure_certificate(certificate, certificate_digests, manifest, result));
                effective_https = true;
                break;
            }
        }
    }
    return effective_https;
}

inline void append_website_origins(const service::website_config::website_config_data& config,
    flexedge::node::v2::Website& website) {
    for (const auto& input : config.origins_) {
        auto* origin = website.add_origins();
        origin->set_id(input.id_);
        origin->set_group(input.group_);
        origin->set_protocol(input.protocol_);
        origin->set_host(input.host_);
        origin->set_port(static_cast<std::uint32_t>(input.port_));
        origin->set_role(input.role_);
        origin->set_weight(static_cast<std::uint32_t>(input.weight_));
        origin->set_enabled(input.status_ == "enabled");
    }
}

inline flexedge::node::v2::DeliveryObject
build_website_object(const deployment_website_source& source, const cluster_deployment_source& deployment,
    std::unordered_map<std::string, std::string>& certificate_digests,
    flexedge::node::v2::ClusterReleaseContent& manifest,
    cluster_release_artifact& result) {
    const std::optional<service::website_config::website_config_data> config =
        service::website_config::parse_stored(source.config_json_);
    if (!config) {
        throw std::runtime_error("website release source is invalid");
    }
    flexedge::node::v2::DeliveryObjectContent object_content;
    auto* website = object_content.mutable_website();
    website->set_id(source.id_);
    if (source.response_cache_) {
        flexedge::cache::to_proto(*source.response_cache_, *website->mutable_response_cache());
    }
    website->set_enabled(source.enabled_);
    website->set_revision(source.revision_);
    website->set_origin_host_header(config->origin_host_header_);
    website->set_origin_http_version(config->origin_http_version_);
    website->set_default_origin_group(config->default_origin_group_);
    website->set_origin_connect_timeout_seconds(
        static_cast<std::uint32_t>(config->origin_connect_timeout_seconds_));
    website->set_origin_read_timeout_seconds(
        static_cast<std::uint32_t>(config->origin_read_timeout_seconds_));
    website->set_pass_client_ip(config->pass_client_ip_);
    website->set_health_check_enabled(config->health_check_enabled_);
    website->set_access_log_enabled(config->access_log_enabled_);
    website->set_access_log_request_headers(config->access_log_request_headers_);
    website->set_access_log_request_body(config->access_log_request_body_);
    website->set_access_log_response_headers(config->access_log_response_headers_);
    website->set_access_log_query_params(config->access_log_query_params_);
    website->set_access_log_cookies(config->access_log_cookies_);
    website->set_access_log_referer(config->access_log_referer_);
    website->set_access_log_user_agent(config->access_log_user_agent_);
    website->set_access_log_client_abort(config->access_log_client_abort_);
    copy_string_values(config->access_log_status_code_ranges_,
        website->mutable_access_log_status_code_ranges());
    website->set_minimum_tls_version(config->minimum_tls_version_);
    website->set_health_check_path(config->health_check_path_);
    website->set_health_check_interval_seconds(
        static_cast<std::uint32_t>(config->health_check_interval_seconds_));
    website->set_health_check_timeout_seconds(
        static_cast<std::uint32_t>(config->health_check_timeout_seconds_));
    website->set_health_check_expected_status(
        static_cast<std::uint32_t>(config->health_check_expected_status_));
    website->set_healthy_threshold(static_cast<std::uint32_t>(config->healthy_threshold_));
    website->set_unhealthy_threshold(static_cast<std::uint32_t>(config->unhealthy_threshold_));
    website->set_response_compression_enabled(config->response_compression_enabled_);
    website->set_response_compression_min_bytes(
        static_cast<std::uint32_t>(config->response_compression_min_bytes_));
    website->set_response_compression_max_bytes(
        static_cast<std::uint32_t>(config->response_compression_max_bytes_));
    copy_string_values(config->response_compression_algorithms_,
        website->mutable_response_compression_algorithms());
    copy_string_values(config->response_compression_mime_types_,
        website->mutable_response_compression_mime_types());
    copy_string_values(config->response_compression_extensions_,
        website->mutable_response_compression_extensions());
    copy_string_values(config->response_compression_excluded_extensions_,
        website->mutable_response_compression_excluded_extensions());
    for (const auto& input : config->route_rules_) {
        auto* rule = website->add_route_rules();
        rule->set_id(input.id_);
        rule->set_name(input.name_);
        rule->set_description(input.description_);
        copy_string_values(input.hostnames_, rule->mutable_hostnames());
        for (const auto& condition : input.conditions_) {
            auto* target = rule->add_conditions();
            target->set_source(condition.source_);
            target->set_name(condition.name_);
            target->set_op(condition.op_);
            target->set_value(condition.value_);
        }
        rule->set_rewrite_mode(input.rewrite_mode_);
        rule->set_query_mode(input.query_mode_);
        rule->set_query_string(input.query_string_);
        rule->set_enabled(input.status_ == "enabled");
        rule->set_match_type(input.match_type_);
        rule->set_path(input.path_);
        copy_string_values(input.methods_, rule->mutable_methods());
        rule->set_action(input.action_);
        rule->set_rewrite_path(input.rewrite_path_);
        rule->set_redirect_url(input.redirect_url_);
        rule->set_redirect_status(static_cast<std::uint32_t>(input.redirect_status_));
        rule->set_origin_group(input.origin_group_);
        for (const auto& header : input.request_headers_) {
            auto* target = rule->add_request_headers();
            target->set_name(header.name_);
            target->set_value(header.value_);
        }
        for (const auto& header : input.response_headers_) {
            auto* target = rule->add_response_headers();
            target->set_name(header.name_);
            target->set_value(header.value_);
        }
    }

    const bool effective_https = append_website_domains(
        source, *config, deployment, certificate_digests, manifest, result, *website);
    website->set_https_enabled(effective_https);
    website->set_force_https(effective_https && config->force_https_);
    website->set_hsts_enabled(effective_https && config->hsts_enabled_);
    append_website_origins(*config, *website);
    // Checked before the object is addressed: nodes reject a release that carries a website
    // breaking these rules.
    try {
        flexedge::node::validate_website(*website);
    } catch (const flexedge::node::invalid_website&) {
        service::common::throw_app_error(dispatch_error::website_protocol_invalid);
    }
    return delivery_object(std::move(object_content));
}

inline cluster_release_artifact build_cluster_release(std::string_view release_id,
    std::int64_t generation,
    const cluster_deployment_source& deployment) {
    cluster_release_artifact result;
    flexedge::node::v2::ClusterReleaseContent manifest;
    manifest.set_cluster_id(deployment.cluster_id_);
    manifest.set_release_id(release_id);
    manifest.set_generation(generation);
    manifest.set_access_domain(deployment.access_domain_);
    manifest.set_enabled(deployment.enabled_);
    manifest.set_schema_version(flexedge::node::cluster_release_schema_version);

    std::unordered_map<std::string, std::string> certificate_digests;
    for (const auto& source : deployment.websites_) {
        auto object = build_website_object(source, deployment, certificate_digests, manifest, result);
        auto* reference = manifest.add_objects();
        reference->set_kind(flexedge::node::v2::OBJECT_KIND_WEBSITE);
        reference->set_digest_sha256(object.digest_sha256());
        result.objects_.push_back(std::move(object));
    }
    result.manifest_ = flexedge::node::pack_artifact<flexedge::node::v2::ClusterReleaseManifest>(manifest);
    return result;
}

}  // namespace service::node_dispatch
