#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
namespace flexedge::testing {
// Tests edit the parsed contents; finalize() packs them into the content-addressed
// `active` state and `objects` the node receives.
struct TestConfig final {
    flexedge::node::v2::NodeSpecContent node_spec;
    flexedge::node::v2::ClusterReleaseContent release;
    std::vector<flexedge::node::v2::DeliveryObjectContent> contents;
    flexedge::node::v2::ActiveState active;
    std::vector<flexedge::node::v2::DeliveryObject> objects;

    flexedge::node::v2::Website* mutableWebsite() {
        for (auto& content : contents) {
            if (content.has_website()) {
                return content.mutable_website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    const flexedge::node::v2::Website& website() const {
        for (const auto& content : contents) {
            if (content.has_website()) {
                return content.website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    flexedge::node::v2::Certificate* mutableCertificate(std::size_t index) {
        for (auto& content : contents) {
            if (!content.has_certificate()) {
                continue;
            }
            if (index == 0) {
                return content.mutable_certificate();
            }
            --index;
        }
        throw std::runtime_error("test certificate object is missing");
    }

    flexedge::node::v2::Certificate* addCertificate() {
        return contents.emplace_back().mutable_certificate();
    }

    void setGeneration(std::int64_t generation) {
        node_spec.set_revision(generation);
        release.set_generation(generation);
        release.set_release_id("release-" + std::to_string(generation));
    }

    void duplicateWebsite() {
        flexedge::node::v2::DeliveryObjectContent duplicate;
        *duplicate.mutable_website() = website();
        contents.push_back(std::move(duplicate));
    }

    void finalize() {
        std::vector<std::string> certificate_digests;
        for (const auto& content : contents) {
            if (content.has_certificate()) {
                certificate_digests.push_back(
                    flexedge::node::artifact_digest(flexedge::node::serialize_artifact(content)));
            }
        }
        auto* website = mutableWebsite();
        for (auto& domain : *website->mutable_domains()) {
            if (!domain.https_enabled() || certificate_digests.empty() ||
                std::ranges::find(certificate_digests, domain.certificate_digest()) !=
                    certificate_digests.end()) {
                continue;
            }
            domain.set_certificate_digest(certificate_digests.front());
        }
        objects.clear();
        release.clear_objects();
        for (const auto& content : contents) {
            auto& object =
                objects.emplace_back(flexedge::node::pack_artifact<flexedge::node::v2::DeliveryObject>(content));
            auto* reference = release.add_objects();
            reference->set_kind(content.has_website() ? flexedge::node::v2::OBJECT_KIND_WEBSITE
                                                      : flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
            reference->set_digest_sha256(object.digest_sha256());
        }
        *active.mutable_node_spec() = flexedge::node::pack_artifact<flexedge::node::v2::NodeSpec>(node_spec);
        *active.mutable_release() =
            flexedge::node::pack_artifact<flexedge::node::v2::ClusterReleaseManifest>(release);
    }
};

inline TestConfig snapshot(bool forceHttps = true) {
    TestConfig value;
    auto* nodeSpec = &value.node_spec;
    nodeSpec->set_node_id("f3384ad8-afbe-4aa4-8fe5-b292809c4e04");
    nodeSpec->set_schema_version(flexedge::node::node_spec_schema_version);
    nodeSpec->set_revision(3);
    nodeSpec->set_enabled(true);
    auto* endpoint = nodeSpec->add_endpoints();
    endpoint->set_id("endpoint-1");
    endpoint->set_ip_address("127.0.0.1");
    endpoint->set_http_port(80);
    endpoint->set_https_port(443);
    auto* release = &value.release;
    release->set_cluster_id("53a74853-e03f-4ec9-9560-a9acdf4fe780");
    release->set_release_id("release-3");
    release->set_generation(3);
    release->set_access_domain("edge.example.com");
    release->set_enabled(true);
    release->set_schema_version(flexedge::node::cluster_release_schema_version);
    flexedge::node::v2::DeliveryObjectContent websiteObject;
    auto* website = websiteObject.mutable_website();
    website->set_id("website-1");
    website->set_enabled(true);
    website->set_revision(2);
    website->set_https_enabled(true);
    website->set_force_https(forceHttps);
    website->set_minimum_tls_version("1.2");
    website->set_origin_connect_timeout_seconds(10);
    website->set_origin_read_timeout_seconds(30);
    website->set_health_check_path("/");
    website->set_access_log_enabled(true);
    website->set_access_log_request_headers(true);
    website->set_access_log_request_body(true);
    website->set_access_log_response_headers(true);
    website->add_access_log_status_code_ranges("2xx");
    website->set_health_check_interval_seconds(10);
    website->set_health_check_timeout_seconds(3);
    website->set_health_check_expected_status(200);
    website->set_healthy_threshold(2);
    website->set_unhealthy_threshold(3);
    website->set_response_compression_enabled(true);
    website->set_response_compression_min_bytes(1024);
    website->set_response_compression_max_bytes(32 * 1024 * 1024);
    website->add_response_compression_algorithms("zstd");
    website->add_response_compression_algorithms("br");
    website->add_response_compression_algorithms("gzip");
    website->add_response_compression_mime_types("text/*");
    website->add_response_compression_mime_types("application/json");
    website->add_response_compression_extensions(".html");
    website->add_response_compression_extensions(".txt");
    website->add_response_compression_excluded_extensions(".apk");
    auto* domain = website->add_domains();
    domain->set_hostname("WWW.Example.COM.");
    domain->set_https_enabled(true);
    auto* httpOnlyDomain = website->add_domains();
    httpOnlyDomain->set_hostname("http.example.com");
    httpOnlyDomain->set_https_enabled(false);
    auto* origin = website->add_origins();
    origin->set_id("origin-1");
    origin->set_protocol("http");
    origin->set_host("192.0.2.10");
    origin->set_port(8080);
    origin->set_role("primary");
    origin->set_weight(100);
    origin->set_enabled(true);
    flexedge::node::v2::DeliveryObjectContent certificateObject;
    auto* certificate = certificateObject.mutable_certificate();
    certificate->set_id("certificate-1");
    certificate->set_certificate_chain_pem("certificate");
    certificate->set_private_key_pem("private-key");
    value.contents.push_back(std::move(certificateObject));
    value.contents.push_back(std::move(websiteObject));
    value.finalize();
    return value;
}

}  // namespace flexedge::testing
