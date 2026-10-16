#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "node/data/route_rules.h"
#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "node/runtime/compression_validation.h"

namespace flexedge::node {

class invalid_hostname final : public std::runtime_error {
public:
    invalid_hostname()
        : std::runtime_error("hostname is empty or exceeds 253 bytes after normalization") {}
};

inline std::string normalize_hostname(std::string_view value) {
    while (!value.empty() && value.back() == '.') {
        value.remove_suffix(1);
    }
    const auto first_colon = value.find(':');
    if (first_colon != std::string_view::npos && first_colon == value.rfind(':')) {
        value = value.substr(0, first_colon);
    }
    if (value.empty() || value.size() > 253) {
        throw invalid_hostname{};
    }
    std::string result(value);
    std::ranges::transform(result, result.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

struct compiled_route final {
    const v2::Website* website_{};
    const v2::Domain* domain_{};
};

class compiled_config final {
public:
    compiled_config(std::shared_ptr<const v2::ActiveState> state,
        std::vector<v2::DeliveryObject> objects)
        : state_(std::move(state)),
          objects_(std::move(objects)) {
        validate_state();
        index_objects();
        validate_endpoints();
        validate_websites();
    }

    // Index pointers refer to objects owned by this immutable snapshot.
    compiled_config(const compiled_config&) = delete;
    compiled_config& operator=(const compiled_config&) = delete;
    compiled_config(compiled_config&&) = delete;
    compiled_config& operator=(compiled_config&&) = delete;

    [[nodiscard]] const v2::NodeSpec& node_spec() const noexcept {
        return state_->node_spec();
    }

    [[nodiscard]] const v2::ClusterReleaseManifest& release() const noexcept {
        return state_->release();
    }

    [[nodiscard]] bool enabled() const noexcept {
        return node_spec().content().enabled() && release().content().enabled();
    }

    [[nodiscard]] const auto& endpoints() const noexcept {
        return node_spec().content().endpoints();
    }

    [[nodiscard]] const std::vector<const v2::Website*>& websites() const noexcept {
        return websites_;
    }

    [[nodiscard]] const v2::Certificate* certificate(std::string_view digest) const noexcept {
        const auto found = certificates_.find(std::string(digest));
        return found == certificates_.end() ? nullptr : found->second;
    }

    [[nodiscard]] const v2::Website* website(std::string_view host) const {
        const auto found = routes_.find(normalize_hostname(host));
        return found == routes_.end() ? nullptr : found->second.website_;
    }

    [[nodiscard]] const v2::Domain* domain(std::string_view host) const {
        const auto found = routes_.find(normalize_hostname(host));
        return found == routes_.end() ? nullptr : found->second.domain_;
    }

    [[nodiscard]] const flexedge::node::route_patterns& route_patterns() const noexcept {
        return route_patterns_;
    }

private:
    void validate_state() const {
        if (!state_ || !state_->has_node_spec() || !state_->has_release()) {
            throw std::runtime_error("node active state is incomplete");
        }
        const auto& spec = node_spec();
        const auto& spec_content = spec.content();
        if (!spec.has_content() || spec_content.node_id().empty() ||
            spec_content.schema_version() != node_spec_schema_version || spec_content.revision() <= 0 ||
            !flexedge::crypto::is_sha256_digest(spec.digest_sha256()) ||
            artifact_digest(spec_content) != spec.digest_sha256()) {
            throw std::runtime_error("invalid node spec");
        }
        const auto& manifest = release();
        const auto& release_content = manifest.content();
        if (!manifest.has_content() || release_content.cluster_id().empty() ||
            release_content.release_id().empty() || release_content.generation() <= 0 ||
            !supported_cluster_release_schema(release_content.schema_version()) ||
            release_content.access_domain().empty() || !flexedge::crypto::is_sha256_digest(manifest.digest_sha256()) ||
            artifact_digest(release_content) != manifest.digest_sha256()) {
            throw std::runtime_error("invalid cluster release manifest");
        }
    }

    void index_objects() {
        std::unordered_map<std::string, v2::ObjectKind> expected;
        expected.reserve(static_cast<std::size_t>(release().content().objects_size()));
        for (const auto& reference : release().content().objects()) {
            if (!flexedge::crypto::is_sha256_digest(reference.digest_sha256()) ||
                (reference.kind() != v2::OBJECT_KIND_WEBSITE &&
                    reference.kind() != v2::OBJECT_KIND_CERTIFICATE) ||
                !expected.emplace(reference.digest_sha256(), reference.kind()).second) {
                throw std::runtime_error("invalid object reference in release manifest");
            }
        }
        if (expected.size() != objects_.size()) {
            throw std::runtime_error("release object set is incomplete");
        }
        std::unordered_set<std::string> received;
        received.reserve(objects_.size());
        for (const auto& object : objects_) {
            const auto expected_object = expected.find(object.digest_sha256());
            if (expected_object == expected.end() || !object.has_content() ||
                artifact_digest(object.content()) != object.digest_sha256() ||
                !received.emplace(object.digest_sha256()).second) {
                throw std::runtime_error("invalid content-addressed release object");
            }
            if (object.content().has_website() &&
                expected_object->second == v2::OBJECT_KIND_WEBSITE) {
                websites_.push_back(&object.content().website());
            } else if (object.content().has_certificate() &&
                       expected_object->second == v2::OBJECT_KIND_CERTIFICATE) {
                certificates_.emplace(object.digest_sha256(), &object.content().certificate());
            } else {
                throw std::runtime_error("release object kind does not match manifest");
            }
        }
    }

    void validate_endpoints() const {
        std::unordered_set<std::string> endpoint_bindings;
        for (const auto& endpoint : endpoints()) {
            if (endpoint.id().empty() || endpoint.ip_address().empty() ||
                endpoint.http_port() == 0 || endpoint.http_port() > 65535 ||
                endpoint.https_port() == 0 || endpoint.https_port() > 65535 ||
                endpoint.http_port() == endpoint.https_port()) {
                throw std::runtime_error("invalid node endpoint");
            }
            const auto prefix = endpoint.ip_address() + ':';
            if (!endpoint_bindings.emplace(prefix + std::to_string(endpoint.http_port())).second ||
                !endpoint_bindings.emplace(prefix + std::to_string(endpoint.https_port())).second) {
                throw std::runtime_error("duplicate node endpoint binding");
            }
        }
    }

    static void validate_website_basics(const v2::Website& website) {
        if (website.id().empty() || website.revision() <= 0 || website.domains().empty()) {
            throw std::runtime_error("invalid enabled website");
        }
        if (website.origin_connect_timeout_seconds() == 0 ||
            website.origin_read_timeout_seconds() == 0) {
            throw std::runtime_error("enabled website requires finite origin timeouts");
        }
        if (website.health_check_enabled() &&
            (website.health_check_path().empty() || website.health_check_path().front() != '/' ||
                website.health_check_interval_seconds() == 0 ||
                website.health_check_timeout_seconds() == 0 ||
                (website.health_check_expected_status() != 0 &&
                    website.health_check_expected_status() < 100) ||
                website.health_check_expected_status() > 599 || website.healthy_threshold() == 0 ||
                website.unhealthy_threshold() == 0)) {
            throw std::runtime_error("invalid origin health check policy");
        }
        if (website.force_https() && !website.https_enabled()) {
            throw std::runtime_error("force_https requires https_enabled");
        }
        if (website.hsts_enabled() && !website.https_enabled()) {
            throw std::runtime_error("HTTPS feature requires https_enabled");
        }
        if (website.https_enabled() && website.minimum_tls_version() != "1.2" &&
            website.minimum_tls_version() != "1.3") {
            throw std::runtime_error("unsupported minimum TLS version");
        }
        validate_access_log(website);
        validate_response_compression(website);
    }

    static void validate_website_origins(const v2::Website& website) {
        std::unordered_set<std::string_view> enabled_groups;
        std::unordered_set<std::string_view> primary_groups;
        for (const auto& origin : website.origins()) {
            if (!origin.enabled()) {
                continue;
            }
            if (origin.id().empty() || origin.host().empty() || origin.port() == 0 ||
                origin.port() > 65535 ||
                (origin.protocol() != "http" && origin.protocol() != "https") ||
                (origin.role() != "primary" && origin.role() != "backup") || origin.weight() == 0) {
                throw std::runtime_error("invalid enabled website origin");
            }
            const auto group = origin.group().empty() ? std::string_view{"default"}
                                                      : std::string_view(origin.group());
            enabled_groups.emplace(group);
            if (origin.role() == "primary") {
                primary_groups.emplace(group);
            }
        }
        const auto default_group = website.default_origin_group().empty()
                                       ? std::string_view{"default"}
                                       : std::string_view(website.default_origin_group());
        if (enabled_groups.empty() || enabled_groups != primary_groups ||
            !enabled_groups.contains(default_group)) {
            throw std::runtime_error("enabled website requires an enabled origin");
        }
    }

    void validate_website_domains(const v2::Website& website) {
        bool has_https_domain = false;
        for (const auto& domain : website.domains()) {
            if (domain.https_enabled() && !website.https_enabled()) {
                throw std::runtime_error("HTTPS domain requires HTTPS website");
            }
            if (domain.https_enabled() && (domain.certificate_digest().empty() ||
                                              certificate(domain.certificate_digest()) == nullptr)) {
                throw std::runtime_error("HTTPS domain requires certificate material");
            }
            has_https_domain = has_https_domain || domain.https_enabled();
            const auto hostname = normalize_hostname(domain.hostname());
            const auto [_, inserted] =
                routes_.emplace(hostname, compiled_route{.website_ = &website, .domain_ = &domain});
            if (!inserted) {
                throw std::runtime_error("duplicate hostname in node config");
            }
        }
        if (website.https_enabled() && !has_https_domain) {
            throw std::runtime_error("HTTPS website requires an HTTPS domain");
        }
    }

    void validate_websites() {
        for (const auto* website : websites_) {
            if (!website->enabled()) {
                continue;
            }
            validate_website_basics(*website);
            validate_route_rules(*website, &route_patterns_);
            validate_website_origins(*website);
            validate_website_domains(*website);
        }
    }

    static void validate_access_log(const v2::Website& website) {
        std::unordered_set<std::string> ranges;
        for (const auto& value : website.access_log_status_code_ranges()) {
            if ((value != "1xx" && value != "2xx" && value != "3xx" && value != "4xx" &&
                    value != "5xx") ||
                !ranges.emplace(value).second) {
                throw std::runtime_error("invalid access log status code range");
            }
        }
    }

    std::shared_ptr<const v2::ActiveState> state_;
    std::vector<v2::DeliveryObject> objects_;
    std::vector<const v2::Website*> websites_;
    std::unordered_map<std::string, const v2::Certificate*> certificates_;
    std::unordered_map<std::string, compiled_route> routes_;
    flexedge::node::route_patterns route_patterns_;
};

}  // namespace flexedge::node
