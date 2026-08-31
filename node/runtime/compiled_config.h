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

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "node/data/route_rules.h"

namespace flexedge::node {

inline std::string normalizeHostname(std::string_view value) {
    while (!value.empty() && value.back() == '.') {
        value.remove_suffix(1);
    }
    const auto firstColon = value.find(':');
    if (firstColon != std::string_view::npos && firstColon == value.rfind(':')) {
        value = value.substr(0, firstColon);
    }
    if (value.empty() || value.size() > 253) {
        throw std::runtime_error("invalid hostname in node config");
    }
    std::string result(value);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

struct CompiledRoute final {
    const v2::Website* website{};
    const v2::Domain* domain{};
};

class CompiledConfig final {
  public:
    CompiledConfig(std::shared_ptr<const v2::ActiveState> state,
                   std::vector<v2::DeliveryObject> objects)
        : state_(std::move(state)), objects_(std::move(objects)) {
        validateState();
        indexObjects();
        validateEndpoints();
        validateWebsites();
    }

    [[nodiscard]] const v2::NodeSpec& nodeSpec() const noexcept { return state_->node_spec(); }

    [[nodiscard]] const v2::ClusterReleaseManifest& release() const noexcept {
        return state_->release();
    }

    [[nodiscard]] bool enabled() const noexcept {
        return nodeSpec().content().enabled() && release().content().enabled();
    }

    [[nodiscard]] const auto& endpoints() const noexcept {
        return nodeSpec().content().endpoints();
    }

    [[nodiscard]] const std::vector<const v2::Website*>& websites() const noexcept {
        return websites_;
    }

    [[nodiscard]] const v2::Certificate* certificate(std::string_view digest) const noexcept {
        const auto found = certificates_.find(std::string(digest));
        return found == certificates_.end() ? nullptr : found->second;
    }

    [[nodiscard]] const v2::Website* website(std::string_view host) const {
        const auto found = routes_.find(normalizeHostname(host));
        return found == routes_.end() ? nullptr : found->second.website;
    }

    [[nodiscard]] const v2::Domain* domain(std::string_view host) const {
        const auto found = routes_.find(normalizeHostname(host));
        return found == routes_.end() ? nullptr : found->second.domain;
    }

  private:
    void validateState() const {
        if (!state_ || !state_->has_node_spec() || !state_->has_release()) {
            throw std::runtime_error("node active state is incomplete");
        }
        const auto& spec = nodeSpec();
        const auto& specContent = spec.content();
        if (!spec.has_content() || specContent.node_id().empty() ||
            specContent.schema_version() != kNodeSpecSchemaVersion || specContent.revision() <= 0 ||
            !isSha256Digest(spec.digest_sha256()) ||
            artifactDigest(specContent) != spec.digest_sha256()) {
            throw std::runtime_error("invalid node spec");
        }
        const auto& manifest = release();
        const auto& releaseContent = manifest.content();
        if (!manifest.has_content() || releaseContent.cluster_id().empty() ||
            releaseContent.release_id().empty() || releaseContent.generation() <= 0 ||
            releaseContent.schema_version() != kClusterReleaseSchemaVersion ||
            releaseContent.access_domain().empty() || !isSha256Digest(manifest.digest_sha256()) ||
            artifactDigest(releaseContent) != manifest.digest_sha256()) {
            throw std::runtime_error("invalid cluster release manifest");
        }
    }

    void indexObjects() {
        std::unordered_map<std::string, v2::ObjectKind> expected;
        expected.reserve(static_cast<std::size_t>(release().content().objects_size()));
        for (const auto& reference : release().content().objects()) {
            if (!isSha256Digest(reference.digest_sha256()) ||
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
            const auto expectedObject = expected.find(object.digest_sha256());
            if (expectedObject == expected.end() || !object.has_content() ||
                artifactDigest(object.content()) != object.digest_sha256() ||
                !received.emplace(object.digest_sha256()).second) {
                throw std::runtime_error("invalid content-addressed release object");
            }
            if (object.content().has_website() &&
                expectedObject->second == v2::OBJECT_KIND_WEBSITE) {
                websites_.push_back(&object.content().website());
            } else if (object.content().has_certificate() &&
                       expectedObject->second == v2::OBJECT_KIND_CERTIFICATE) {
                certificates_.emplace(object.digest_sha256(), &object.content().certificate());
            } else {
                throw std::runtime_error("release object kind does not match manifest");
            }
        }
    }

    void validateEndpoints() const {
        std::unordered_set<std::string> endpointBindings;
        for (const auto& endpoint : endpoints()) {
            if (endpoint.id().empty() || endpoint.ip_address().empty() ||
                endpoint.http_port() == 0 || endpoint.http_port() > 65535 ||
                endpoint.https_port() == 0 || endpoint.https_port() > 65535 ||
                endpoint.http_port() == endpoint.https_port()) {
                throw std::runtime_error("invalid node endpoint");
            }
            const auto prefix = endpoint.ip_address() + ':';
            if (!endpointBindings.emplace(prefix + std::to_string(endpoint.http_port())).second ||
                !endpointBindings.emplace(prefix + std::to_string(endpoint.https_port())).second) {
                throw std::runtime_error("duplicate node endpoint binding");
            }
        }
    }

    static void validateWebsiteBasics(const v2::Website& website) {
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
        if ((website.hsts_enabled() || website.http2_enabled()) && !website.https_enabled()) {
            throw std::runtime_error("HTTPS feature requires https_enabled");
        }
        if (website.https_enabled() && website.minimum_tls_version() != "1.2" &&
            website.minimum_tls_version() != "1.3") {
            throw std::runtime_error("unsupported minimum TLS version");
        }
        validateAccessLog(website);
        validateCompression(website);
        validateRouteRules(website);
    }

    static void validateWebsiteOrigins(const v2::Website& website) {
        std::unordered_set<std::string_view> enabledGroups;
        std::unordered_set<std::string_view> primaryGroups;
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
            enabledGroups.emplace(group);
            if (origin.role() == "primary") {
                primaryGroups.emplace(group);
            }
        }
        const auto defaultGroup = website.default_origin_group().empty()
                                      ? std::string_view{"default"}
                                      : std::string_view(website.default_origin_group());
        if (enabledGroups.empty() || enabledGroups != primaryGroups ||
            !enabledGroups.contains(defaultGroup)) {
            throw std::runtime_error("enabled website requires an enabled origin");
        }
    }

    void validateWebsiteDomains(const v2::Website& website) {
        bool hasHttpsDomain = false;
        for (const auto& domain : website.domains()) {
            if (domain.https_enabled() && !website.https_enabled()) {
                throw std::runtime_error("HTTPS domain requires HTTPS website");
            }
            if (domain.https_enabled() && (domain.certificate_digest().empty() ||
                                           certificate(domain.certificate_digest()) == nullptr)) {
                throw std::runtime_error("HTTPS domain requires certificate material");
            }
            hasHttpsDomain = hasHttpsDomain || domain.https_enabled();
            const auto hostname = normalizeHostname(domain.hostname());
            const auto [_, inserted] =
                routes_.emplace(hostname, CompiledRoute{.website = &website, .domain = &domain});
            if (!inserted) {
                throw std::runtime_error("duplicate hostname in node config");
            }
        }
        if (website.https_enabled() && !hasHttpsDomain) {
            throw std::runtime_error("HTTPS website requires an HTTPS domain");
        }
    }

    void validateWebsites() {
        for (const auto* website : websites_) {
            if (!website->enabled()) {
                continue;
            }
            validateWebsiteBasics(*website);
            validateWebsiteOrigins(*website);
            validateWebsiteDomains(*website);
        }
    }

    static void validateCompressionLimits(const v2::Website& website) {
        if (website.response_compression_enabled() &&
            (website.response_compression_min_bytes() < 256 ||
             website.response_compression_min_bytes() > 1024 * 1024 ||
             (website.response_compression_max_bytes() != 0 &&
              website.response_compression_max_bytes() <
                  website.response_compression_min_bytes()) ||
             website.response_compression_max_bytes() > 64 * 1024 * 1024 ||
             website.response_compression_algorithms().empty())) {
            throw std::runtime_error("invalid response compression policy");
        }
    }

    static void validateCompressionAlgorithms(const v2::Website& website) {
        std::unordered_set<std::string> algorithms;
        for (const auto& algorithm : website.response_compression_algorithms()) {
            if ((algorithm != "br" && algorithm != "zstd" && algorithm != "gzip") ||
                !algorithms.emplace(algorithm).second) {
                throw std::runtime_error("invalid response compression algorithm");
            }
        }
    }

    static bool validCompressionMimeType(std::string_view value) noexcept {
        const auto validToken = [](std::string_view segment) {
            return !segment.empty() && std::ranges::all_of(segment, [](unsigned char ch) {
                return std::isalnum(ch) || ch == '!' || ch == '#' || ch == '$' || ch == '&' ||
                       ch == '^' || ch == '_' || ch == '.' || ch == '+' || ch == '-';
            });
        };
        const auto slash = value.find('/');
        if (slash == std::string_view::npos ||
            value.find('/', slash + 1) != std::string_view::npos) {
            return false;
        }
        const auto type = value.substr(0, slash);
        const auto subtype = value.substr(slash + 1);
        return validToken(type) && (subtype == "*" || validToken(subtype));
    }

    template <typename Values>
    static void validateCompressionMatchValues(const Values& values, bool extension,
                                               bool mimeType) {
        if (values.size() > 32) {
            throw std::runtime_error("too many response compression match values");
        }
        std::unordered_set<std::string> seen;
        for (const auto& value : values) {
            if (value.empty() || value.size() > 127 ||
                std::ranges::any_of(value,
                                    [](unsigned char ch) { return std::isspace(ch) != 0; }) ||
                (extension && value.front() != '.') ||
                (mimeType && !validCompressionMimeType(value)) || !seen.emplace(value).second) {
                throw std::runtime_error("invalid response compression match value");
            }
        }
    }

    static void validateCompression(const v2::Website& website) {
        validateCompressionLimits(website);
        validateCompressionAlgorithms(website);
        validateCompressionMatchValues(website.response_compression_mime_types(), false, true);
        validateCompressionMatchValues(website.response_compression_extensions(), true, false);
        validateCompressionMatchValues(website.response_compression_excluded_extensions(), true,
                                       false);
    }

    static void validateAccessLog(const v2::Website& website) {
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
    std::unordered_map<std::string, CompiledRoute> routes_;
};

} // namespace flexedge::node
