#include "service/domains/agent/agent_protocol.h"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

#include "service/domains/agent/agent_report_mapper.h"
#include "service/domains/agent/release_manifest.h"

#define REQUIRE(condition)                                      \
    do {                                                        \
        if (!(condition)) throw std::runtime_error(#condition); \
    } while (false)

int main() {
    try {
        namespace proto = flexedge::node::v2;
        const std::string id = "01234567-89ab-cdef-0123-456789abcdef";
        const std::string digest(64, 'a');
        proto::ClientEnvelope envelope;
        envelope.set_request_id("auth-1");
        auto& auth = *envelope.mutable_authenticate();
        auth.set_node_id("0123456789abcdef0123456789abcdef");
        auth.set_secret(std::string(32, '!'));
        auth.set_session_purpose(proto::AGENT_SESSION_PURPOSE_CONTROL);
        REQUIRE(service::agent::valid_authentication_envelope(envelope));
        auth.set_active_release_id(id);
        REQUIRE(!service::agent::valid_authenticate(auth));
        auth.set_active_manifest_digest(digest);
        REQUIRE(service::agent::valid_authenticate(auth));
        auth.set_session_purpose(proto::AGENT_SESSION_PURPOSE_LOG_INGEST);
        REQUIRE(service::agent::valid_authenticate(auth));
        auth.set_secret(std::string(32, ' '));
        REQUIRE(!service::agent::valid_authenticate(auth));

        proto::Heartbeat heartbeat;
        heartbeat.set_node_id(id);
        heartbeat.set_applied_node_spec_revision(7);
        heartbeat.set_active_release_id(id);
        heartbeat.set_active_manifest_digest(digest);
        heartbeat.set_cpu_usage(0.5);
        heartbeat.set_memory_usage(0.25);
        heartbeat.set_queued_log_events(42);
        auto& origin = *heartbeat.add_origin_health();
        origin.set_website_id(id);
        origin.set_origin_id(id);
        origin.set_status("healthy");
        origin.set_latency_millis(17);
        REQUIRE(service::agent::valid_heartbeat(heartbeat));
        const auto report = service::agent::to_heartbeat_report(heartbeat);
        REQUIRE(report.node_id_ == id && report.applied_node_spec_revision_ == 7);
        REQUIRE(report.active_manifest_digest_ == digest && report.queued_log_events_ == 42);
        REQUIRE(report.cpu_usage_ == 0.5 && report.memory_usage_ == 0.25);
        REQUIRE(report.origin_health_.size() == 1 && report.origin_health_[0].latency_millis_ == 17);
        // Nodes before 0.4.1 never send connected_ip_count: absent stays absent, 0 stays 0.
        REQUIRE(!report.connected_ip_count_.has_value());
        heartbeat.set_connection_count(2);
        heartbeat.set_connected_ip_count(0);
        REQUIRE(service::agent::valid_heartbeat(heartbeat));
        REQUIRE(service::agent::to_heartbeat_report(heartbeat).connected_ip_count_ == 0);
        // Sampled apart from the connection count, so a larger value is still accepted.
        heartbeat.set_connected_ip_count(3);
        REQUIRE(service::agent::valid_heartbeat(heartbeat));
        REQUIRE(service::agent::to_heartbeat_report(heartbeat).connected_ip_count_ == 3);
        heartbeat.set_connected_ip_count(-1);
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        heartbeat.clear_connected_ip_count();
        REQUIRE(service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_queued_log_events((std::numeric_limits<std::uint64_t>::max)());
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_queued_log_events(0);
        heartbeat.set_load_1m(std::numeric_limits<double>::infinity());
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_load_1m(-std::numeric_limits<double>::infinity());
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_load_1m(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_load_1m((std::numeric_limits<double>::max)());
        REQUIRE(service::agent::valid_heartbeat(heartbeat));
        heartbeat.set_load_1m(0);
        heartbeat.set_cpu_usage(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(!service::agent::valid_heartbeat(heartbeat));
        proto::ClusterReleaseManifest manifest;
        manifest.mutable_content()->set_release_id(id);
        manifest.mutable_content()->set_cluster_id("cluster");
        manifest.set_digest_sha256(flexedge::node::artifact_digest(manifest.content()));
        const auto bytes = flexedge::node::serialize_artifact(manifest);
        const auto hash = manifest.digest_sha256();
        const auto parsed = service::agent::parse_release_manifest(bytes, hash, id, "cluster");
        REQUIRE(parsed && parsed->content().release_id() == id);
        REQUIRE(!service::agent::parse_release_manifest(bytes, hash, id, "foreign"));
        REQUIRE(!service::agent::parse_release_manifest(bytes, hash, "foreign", "cluster"));
        REQUIRE(!service::agent::parse_release_manifest(bytes, digest, id, "cluster"));
        REQUIRE(!service::agent::parse_release_manifest(std::string(1, '\xff'), hash, id, "cluster"));
        manifest.mutable_content()->set_enabled(true);
        REQUIRE(!service::agent::parse_release_manifest(flexedge::node::serialize_artifact(manifest), hash, id, "cluster"));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
