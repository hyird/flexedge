#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/route_policy.h"
#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "node/proto/website_validation.h"

namespace flexedge::node {

struct compiled_route final {
    const v2::Website* website_{};
    const v2::Domain* domain_{};
};

class compiled_config final {
public:
    // Verifies every content-addressed part against the bytes it carries, then parses and
    // validates the parsed contents this snapshot owns.
    compiled_config(const v2::ActiveState& state, const std::vector<v2::DeliveryObject>& objects) {
        open_state(state);
        index_objects(objects);
        validate_endpoints();
        validate_websites();
    }

    // Index pointers refer to objects owned by this immutable snapshot.
    compiled_config(const compiled_config&) = delete;
    compiled_config& operator=(const compiled_config&) = delete;
    compiled_config(compiled_config&&) = delete;
    compiled_config& operator=(compiled_config&&) = delete;

    [[nodiscard]] const v2::NodeSpecContent& node_spec() const noexcept {
        return node_spec_;
    }

    [[nodiscard]] const std::string& node_spec_digest() const noexcept {
        return node_spec_digest_;
    }

    [[nodiscard]] const v2::ClusterReleaseContent& release() const noexcept {
        return release_;
    }

    [[nodiscard]] const std::string& release_digest() const noexcept {
        return release_digest_;
    }

    [[nodiscard]] bool enabled() const noexcept {
        return node_spec_.enabled() && release_.enabled();
    }

    [[nodiscard]] const auto& endpoints() const noexcept {
        return node_spec_.endpoints();
    }

    [[nodiscard]] const std::vector<const v2::Website*>& websites() const noexcept {
        return websites_;
    }

    [[nodiscard]] const v2::Certificate* certificate(std::string_view digest) const noexcept {
        const auto found = certificates_.find(std::string(digest));
        return found == certificates_.end() ? nullptr : found->second;
    }

    [[nodiscard]] const v2::Website* website(std::string_view host) const {
        const auto found = flexedge::route_policy::find_hostname(routes_,
            flexedge::route_policy::hostname(host));
        return found == routes_.end() ? nullptr : found->second.website_;
    }

    [[nodiscard]] const v2::Domain* domain(std::string_view host) const {
        const auto found = flexedge::route_policy::find_hostname(routes_,
            flexedge::route_policy::hostname(host));
        return found == routes_.end() ? nullptr : found->second.domain_;
    }

private:
    void open_state(const v2::ActiveState& state) {
        if (!state.has_node_spec() || !state.has_release()) {
            throw std::runtime_error("node active state is incomplete");
        }
        auto opened = flexedge::node::open_state(state);
        if (!opened) {
            throw std::runtime_error("invalid node active state");
        }
        auto& [spec, release] = *opened;
        if (spec.node_id().empty() || spec.schema_version() != node_spec_schema_version ||
            spec.revision() <= 0) {
            throw std::runtime_error("invalid node spec");
        }
        if (release.cluster_id().empty() || release.release_id().empty() ||
            release.generation() <= 0 ||
            !supported_cluster_release_schema(release.schema_version()) ||
            release.access_domain().empty()) {
            throw std::runtime_error("invalid cluster release manifest");
        }
        node_spec_digest_ = state.node_spec().digest_sha256();
        node_spec_ = std::move(spec);
        release_digest_ = state.release().digest_sha256();
        release_ = std::move(release);
    }

    void index_objects(const std::vector<v2::DeliveryObject>& objects) {
        std::unordered_map<std::string_view, v2::ObjectKind> expected;
        expected.reserve(static_cast<std::size_t>(release_.objects_size()));
        for (const auto& reference : release_.objects()) {
            if (!flexedge::crypto::is_sha256_digest(reference.digest_sha256()) ||
                (reference.kind() != v2::OBJECT_KIND_WEBSITE &&
                    reference.kind() != v2::OBJECT_KIND_CERTIFICATE) ||
                !expected.emplace(reference.digest_sha256(), reference.kind()).second) {
                throw std::runtime_error("invalid object reference in release manifest");
            }
        }
        if (expected.size() != objects.size()) {
            throw std::runtime_error("release object set is incomplete");
        }
        // Reserved up front: the index keeps pointers into these contents.
        contents_.reserve(objects.size());
        std::unordered_set<std::string_view> received;
        received.reserve(objects.size());
        for (const auto& object : objects) {
            const auto expected_object = expected.find(object.digest_sha256());
            std::optional<v2::DeliveryObjectContent> content;
            if (expected_object != expected.end()) {
                content = open_artifact<v2::DeliveryObjectContent>(object.digest_sha256(),
                    object.content());
            }
            if (!content || !received.emplace(object.digest_sha256()).second) {
                throw std::runtime_error("invalid content-addressed release object");
            }
            const auto& stored = contents_.emplace_back(std::move(*content));
            if (stored.has_website() && expected_object->second == v2::OBJECT_KIND_WEBSITE) {
                websites_.push_back(&stored.website());
            } else if (stored.has_certificate() &&
                       expected_object->second == v2::OBJECT_KIND_CERTIFICATE) {
                certificates_.emplace(object.digest_sha256(), &stored.certificate());
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

    // Certificate material and hostnames span the release; validate_website covers the rest.
    void index_website_domains(const v2::Website& website) {
        for (const auto& domain : website.domains()) {
            if (domain.https_enabled() && certificate(domain.certificate_digest()) == nullptr) {
                throw std::runtime_error("HTTPS domain requires certificate material");
            }
            const auto [_, inserted] =
                routes_.emplace(flexedge::route_policy::hostname(domain.hostname()),
                    compiled_route{.website_ = &website, .domain_ = &domain});
            if (!inserted) {
                throw std::runtime_error("duplicate hostname in node config");
            }
        }
    }

    void validate_websites() {
        for (const auto* website : websites_) {
            if (!website->enabled()) {
                continue;
            }
            validate_website(*website);
            index_website_domains(*website);
        }
    }

    std::string node_spec_digest_;
    v2::NodeSpecContent node_spec_;
    std::string release_digest_;
    v2::ClusterReleaseContent release_;
    std::vector<v2::DeliveryObjectContent> contents_;
    std::vector<const v2::Website*> websites_;
    std::unordered_map<std::string, const v2::Certificate*> certificates_;
    std::unordered_map<std::string, compiled_route> routes_;
};

}  // namespace flexedge::node
