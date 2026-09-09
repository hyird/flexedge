#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>



#include "common/sha256.h"
#include "node/proto/credential_validation.h"
#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/uuid.h"


namespace service::agent {

inline bool validReleaseId(std::string_view value) {
    return service::common::parseUuid(value).has_value();
}

inline bool validRequestId(std::string_view value) {
    return !value.empty() && value.size() <= 96 && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-';
    });
}

inline bool validAuthenticate(const flexedge::node::v2::Authenticate& value) {
    const bool validNodeId = flexedge::node::validCredentialNodeId(value.node_id());
    const bool validSecret = flexedge::node::validCredentialSecret(value.secret());
    const bool hasActiveRelease = !value.active_release_id().empty();
    const bool validSessionPurpose =
        value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_CONTROL ||
        value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST;
    return validNodeId && validSecret && value.applied_node_spec_revision() >= 0 &&
           hasActiveRelease == !value.active_manifest_digest().empty() &&
           (!hasActiveRelease ||
            (validReleaseId(value.active_release_id()) &&
             flexedge::crypto::isSha256Digest(value.active_manifest_digest()))) &&
           value.agent_version().size() <= 64 && validSessionPurpose;
}

inline bool validAuthenticationEnvelope(const flexedge::node::v2::ClientEnvelope& envelope) {
    return validRequestId(envelope.request_id()) && envelope.has_authenticate() &&
           validAuthenticate(envelope.authenticate());
}

inline bool validHeartbeat(const flexedge::node::v2::Heartbeat& value) {
    return service::common::parseUuid(value.node_id()) && value.applied_node_spec_revision() >= 1 &&
           validReleaseId(value.active_release_id()) &&
           flexedge::crypto::isSha256Digest(value.active_manifest_digest()) &&
           value.agent_version().size() <= 64 && value.cpu_usage() >= 0 && value.cpu_usage() <= 1 &&
           value.memory_usage() >= 0 && value.memory_usage() <= 1 && value.traffic_out_bps() >= 0 &&
           value.connection_count() >= 0 && std::isfinite(value.load_1m()) && value.load_1m() >= 0 &&
           value.queued_log_events() <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
           value.dropped_log_events() <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
           value.health().size() <= 64 && value.last_error().size() <= 1000 &&
           value.origin_health_size() <= 10000 &&
           std::ranges::all_of(value.origin_health(), [](const auto& item) {
               return service::common::parseUuid(item.website_id()) &&
                      service::common::parseUuid(item.origin_id()) &&
                      (item.status() == "healthy" || item.status() == "unhealthy" ||
                       item.status() == "unknown") &&
                      item.checked_at_unix_millis() >= 0 && item.latency_millis() <= 600000 &&
                      item.last_error().size() <= 1000;
           });
}

inline bool validObjectRequest(const flexedge::node::v2::ObjectRequest& value) {
    if (!service::common::parseUuid(value.node_id()) || !validReleaseId(value.release_id()) ||
        value.digest_sha256().empty() || value.digest_sha256().size() > 64) {
        return false;
    }
    return std::ranges::all_of(value.digest_sha256(), [](const auto& digest) {
        return flexedge::crypto::isSha256Digest(digest);
    });
}

inline bool validNodeReleaseRequest(const flexedge::node::v2::NodeReleaseRequest& value) {
    return service::common::parseUuid(value.node_id()) &&
           flexedge::crypto::isSha256Digest(value.digest_sha256()) &&
           value.offset() < flexedge::node::kMaximumNodeReleaseBytes;
}

inline bool validApplyPhase(flexedge::node::v2::ApplyPhase phase) {
    return phase == flexedge::node::v2::APPLY_PHASE_STAGE ||
           phase == flexedge::node::v2::APPLY_PHASE_VALIDATE ||
           phase == flexedge::node::v2::APPLY_PHASE_ACTIVATE;
}

inline bool validApplyResult(const flexedge::node::v2::ApplyResult& value) {
    if (!service::common::parseUuid(value.node_id()) || value.node_spec_revision() < 1 ||
        !validReleaseId(value.release_id()) ||
        !flexedge::crypto::isSha256Digest(value.manifest_digest()) ||
        value.error_code().size() > 64 || value.error().size() > 1000) {
        return false;
    }
    return value.applied() ? value.failed_phase() == flexedge::node::v2::APPLY_PHASE_UNSPECIFIED &&
                                 value.error_code().empty() && value.error().empty()
                           : validApplyPhase(value.failed_phase()) && !value.error_code().empty() &&
                                 !value.error().empty();
}

} // namespace service::agent
