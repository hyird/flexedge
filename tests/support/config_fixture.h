#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include <utility>
#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
namespace flexedge::testing {
struct TestConfig final {
    flexedge::node::v2::ActiveState active;
    std::vector<flexedge::node::v2::DeliveryObject> objects;

    flexedge::node::v2::Website* mutableWebsite() {
        for (auto& object : objects) {
            if (object.content().has_website()) {
                return object.mutable_content()->mutable_website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    const flexedge::node::v2::Website& website() const {
        for (const auto& object : objects) {
            if (object.content().has_website()) {
                return object.content().website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    flexedge::node::v2::Certificate* mutableCertificate(std::size_t index) {
        for (auto& object : objects) {
            if (!object.content().has_certificate()) {
                continue;
            }
            if (index == 0) {
                return object.mutable_content()->mutable_certificate();
            }
            --index;
        }
        throw std::runtime_error("test certificate object is missing");
    }

    flexedge::node::v2::Certificate* addCertificate() {
        flexedge::node::v2::DeliveryObject object;
        object.mutable_content()->mutable_certificate();
        objects.push_back(std::move(object));
        return objects.back().mutable_content()->mutable_certificate();
    }

    void setGeneration(std::int64_t generation) {
        active.mutable_node_spec()->mutable_content()->set_revision(generation);
        active.mutable_release()->mutable_content()->set_generation(generation);
        active.mutable_release()->mutable_content()->set_release_id("release-" +
                                                                    std::to_string(generation));
    }

    void duplicateWebsite() {
        flexedge::node::v2::DeliveryObject duplicate;
        *duplicate.mutable_content()->mutable_website() = website();
        objects.push_back(std::move(duplicate));
    }

    void finalize() {
        for (auto& object : objects) {
            if (object.content().has_certificate()) {
                object.set_digest_sha256(flexedge::node::artifactDigest(object.content()));
            }
        }
        auto* website = mutableWebsite();
        for (auto& domain : *website->mutable_domains()) {
            if (!domain.https_enabled()) {
                continue;
            }
            bool resolved = false;
            for (const auto& object : objects) {
                resolved = resolved || (object.content().has_certificate() &&
                                        object.digest_sha256() == domain.certificate_digest());
            }
            if (resolved) {
                continue;
            }
            for (const auto& object : objects) {
                if (object.content().has_certificate()) {
                    domain.set_certificate_digest(object.digest_sha256());
                    break;
                }
            }
        }
        auto* manifest = active.mutable_release()->mutable_content();
        manifest->clear_objects();
        for (auto& object : objects) {
            object.set_digest_sha256(flexedge::node::artifactDigest(object.content()));
            auto* reference = manifest->add_objects();
            reference->set_kind(object.content().has_website()
                                    ? flexedge::node::v2::OBJECT_KIND_WEBSITE
                                    : flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
            reference->set_digest_sha256(object.digest_sha256());
        }
        active.mutable_node_spec()->set_digest_sha256(
            flexedge::node::artifactDigest(active.node_spec().content()));
        active.mutable_release()->set_digest_sha256(flexedge::node::artifactDigest(*manifest));
    }
};

inline TestConfig snapshot(bool forceHttps = true) {
    TestConfig value;
    auto* nodeSpec = value.active.mutable_node_spec()->mutable_content();
    nodeSpec->set_node_id("f3384ad8-afbe-4aa4-8fe5-b292809c4e04");
    nodeSpec->set_schema_version(flexedge::node::kNodeSpecSchemaVersion);
    nodeSpec->set_revision(3);
    nodeSpec->set_enabled(true);
    auto* endpoint = nodeSpec->add_endpoints();
    endpoint->set_id("endpoint-1");
    endpoint->set_ip_address("127.0.0.1");
    endpoint->set_http_port(80);
    endpoint->set_https_port(443);
    auto* release = value.active.mutable_release()->mutable_content();
    release->set_cluster_id("53a74853-e03f-4ec9-9560-a9acdf4fe780");
    release->set_release_id("release-3");
    release->set_generation(3);
    release->set_access_domain("edge.example.com");
    release->set_enabled(true);
    release->set_schema_version(flexedge::node::kClusterReleaseSchemaVersion);
    flexedge::node::v2::DeliveryObject websiteObject;
    auto* website = websiteObject.mutable_content()->mutable_website();
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
    flexedge::node::v2::DeliveryObject certificateObject;
    auto* certificate = certificateObject.mutable_content()->mutable_certificate();
    certificate->set_id("certificate-1");
    certificate->set_certificate_chain_pem("certificate");
    certificate->set_private_key_pem("private-key");
    value.objects.push_back(std::move(certificateObject));
    value.objects.push_back(std::move(websiteObject));
    value.finalize();
    return value;
}


}
