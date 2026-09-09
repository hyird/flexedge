#include <iostream>
#include "service/domains/agent/release_manifest.h"
#include <limits>
#include <stdexcept>
#include <string>
#include "service/domains/agent/agent_protocol.h"
#include "service/domains/agent/agent_report.mapper.h"

#define REQUIRE(condition) \
    do { if (!(condition)) throw std::runtime_error(#condition); } while (false)

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
        REQUIRE(service::agent::validAuthenticationEnvelope(envelope));
        auth.set_active_release_id(id);
        REQUIRE(!service::agent::validAuthenticate(auth));
        auth.set_active_manifest_digest(digest);
        REQUIRE(service::agent::validAuthenticate(auth));
        auth.set_session_purpose(proto::AGENT_SESSION_PURPOSE_LOG_INGEST);
        REQUIRE(service::agent::validAuthenticate(auth));
        auth.set_secret(std::string(32, ' '));
        REQUIRE(!service::agent::validAuthenticate(auth));

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
        REQUIRE(service::agent::validHeartbeat(heartbeat));
        const auto report = service::agent::toHeartbeatReport(heartbeat);
        REQUIRE(report.nodeId == id && report.appliedNodeSpecRevision == 7);
        REQUIRE(report.activeManifestDigest == digest && report.queuedLogEvents == 42);
        REQUIRE(report.cpuUsage == 0.5 && report.memoryUsage == 0.25);
        REQUIRE(report.originHealth.size() == 1 && report.originHealth[0].latencyMillis == 17);
        heartbeat.set_queued_log_events((std::numeric_limits<std::uint64_t>::max)());
        REQUIRE(!service::agent::validHeartbeat(heartbeat));
        heartbeat.set_queued_log_events(0);
        heartbeat.set_load_1m(std::numeric_limits<double>::infinity());
        REQUIRE(!service::agent::validHeartbeat(heartbeat));
        heartbeat.set_load_1m(-std::numeric_limits<double>::infinity());
        REQUIRE(!service::agent::validHeartbeat(heartbeat));
        heartbeat.set_load_1m(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(!service::agent::validHeartbeat(heartbeat));
        heartbeat.set_load_1m((std::numeric_limits<double>::max)());
        REQUIRE(service::agent::validHeartbeat(heartbeat));
        heartbeat.set_load_1m(0);
        heartbeat.set_cpu_usage(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(!service::agent::validHeartbeat(heartbeat));
        proto::ClusterReleaseManifest manifest;
        manifest.mutable_content()->set_release_id(id);
        manifest.mutable_content()->set_cluster_id("cluster");
        manifest.set_digest_sha256(flexedge::node::artifactDigest(manifest.content()));
        const auto bytes = flexedge::node::serializeArtifact(manifest);
        const auto hash = manifest.digest_sha256();
        const auto parsed = service::agent::parseReleaseManifest(bytes, hash, id, "cluster");
        REQUIRE(parsed && parsed->content().release_id() == id);
        REQUIRE(!service::agent::parseReleaseManifest(bytes, hash, id, "foreign"));
        REQUIRE(!service::agent::parseReleaseManifest(bytes, hash, "foreign", "cluster"));
        REQUIRE(!service::agent::parseReleaseManifest(bytes, digest, id, "cluster"));
        REQUIRE(!service::agent::parseReleaseManifest(std::string(1, '\xff'), hash, id, "cluster"));
        manifest.mutable_content()->set_enabled(true);
        REQUIRE(!service::agent::parseReleaseManifest(flexedge::node::serializeArtifact(manifest), hash, id, "cluster"));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
