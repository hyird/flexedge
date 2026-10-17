#pragma once

#include <ruvia/web/websocket.h>

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
#include "node/proto/control_protocol.h"
#include "node/proto/credential_validation.h"
#include "node/proto/edge_control.pb.h"
#include "service/common/uuid.h"

namespace service::node_channel {

inline bool parse_client_envelope(const ruvia::websocket_message& message,
    flexedge::node::v2::ClientEnvelope& envelope) {
    if (!message.binary() ||
        message.payload().size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return envelope.ParseFromArray(message.payload().data(),
        static_cast<int>(message.payload().size()));
}

inline bool valid_release_id(std::string_view value) {
    return service::common::parse_uuid(value).has_value();
}

inline bool valid_request_id(std::string_view value) {
    return !value.empty() && value.size() <= 96 && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '-';
    });
}

inline bool valid_authenticate(const flexedge::node::v2::Authenticate& value) {
    const bool valid_node_id = flexedge::node::valid_credential_node_id(value.node_id());
    const bool valid_secret = flexedge::node::valid_credential_secret(value.secret());
    const bool has_active_release = !value.active_release_id().empty();
    const bool valid_session_purpose =
        value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_CONTROL ||
        value.session_purpose() == flexedge::node::v2::AGENT_SESSION_PURPOSE_LOG_INGEST;
    return valid_node_id && valid_secret && value.applied_node_spec_revision() >= 0 &&
           has_active_release == !value.active_manifest_digest().empty() &&
           (!has_active_release ||
               (valid_release_id(value.active_release_id()) &&
                   flexedge::crypto::is_sha256_digest(value.active_manifest_digest()))) &&
           value.agent_version().size() <= 64 && valid_session_purpose;
}

inline bool valid_authentication_envelope(const flexedge::node::v2::ClientEnvelope& envelope) {
    return valid_request_id(envelope.request_id()) && envelope.has_authenticate() &&
           valid_authenticate(envelope.authenticate());
}

inline bool valid_heartbeat(const flexedge::node::v2::Heartbeat& value) {
    return service::common::parse_uuid(value.node_id()) && value.applied_node_spec_revision() >= 1 &&
           valid_release_id(value.active_release_id()) &&
           flexedge::crypto::is_sha256_digest(value.active_manifest_digest()) &&
           value.agent_version().size() <= 64 && value.cpu_usage() >= 0 && value.cpu_usage() <= 1 &&
           value.memory_usage() >= 0 && value.memory_usage() <= 1 && value.traffic_out_bps() >= 0 &&
           value.connection_count() >= 0 &&
           // Sampled apart from connection_count, so it is not bounded by it.
           (!value.has_connected_ip_count() || value.connected_ip_count() >= 0) &&
           (!value.has_cpu_count() || value.cpu_count() > 0) &&
           (!value.has_memory_total_bytes() || (value.memory_total_bytes() > 0 &&
                                                   value.memory_total_bytes() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))) &&
           (!value.has_disk_total_bytes() || (value.disk_total_bytes() > 0 &&
                                                 value.disk_total_bytes() <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()))) &&
           (!value.has_disk_usage() || (std::isfinite(value.disk_usage()) && value.disk_usage() >= 0 && value.disk_usage() <= 1)) &&
           std::isfinite(value.load_1m()) && value.load_1m() >= 0 &&
           value.queued_log_events() <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
           value.dropped_log_events() <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
           value.health().size() <= 64 && value.last_error().size() <= 1000 &&
           value.origin_health_size() <= 10000 &&
           std::ranges::all_of(value.origin_health(), [](const auto& item) {
               return service::common::parse_uuid(item.website_id()) &&
                      service::common::parse_uuid(item.origin_id()) &&
                      (item.status() == "healthy" || item.status() == "unhealthy" ||
                          item.status() == "unknown") &&
                      item.checked_at_unix_millis() >= 0 && item.latency_millis() <= 600000 &&
                      item.last_error().size() <= 1000;
           });
}

inline bool valid_object_request(const flexedge::node::v2::ObjectRequest& value) {
    if (!service::common::parse_uuid(value.node_id()) || !valid_release_id(value.release_id()) ||
        value.digest_sha256().empty() || value.digest_sha256().size() > 64) {
        return false;
    }
    return std::ranges::all_of(value.digest_sha256(), [](const auto& digest) {
        return flexedge::crypto::is_sha256_digest(digest);
    });
}

inline bool valid_node_release_request(const flexedge::node::v2::NodeReleaseRequest& value) {
    return service::common::parse_uuid(value.node_id()) &&
           flexedge::crypto::is_sha256_digest(value.digest_sha256()) &&
           value.offset() < flexedge::node::maximum_node_release_bytes;
}

inline bool valid_apply_phase(flexedge::node::v2::ApplyPhase phase) {
    return phase == flexedge::node::v2::APPLY_PHASE_STAGE ||
           phase == flexedge::node::v2::APPLY_PHASE_VALIDATE ||
           phase == flexedge::node::v2::APPLY_PHASE_ACTIVATE;
}

inline bool valid_apply_result(const flexedge::node::v2::ApplyResult& value) {
    if (!service::common::parse_uuid(value.node_id()) || value.node_spec_revision() < 1 ||
        !valid_release_id(value.release_id()) ||
        !flexedge::crypto::is_sha256_digest(value.manifest_digest()) ||
        value.error_code().size() > 64 || value.error().size() > 1000) {
        return false;
    }
    return value.applied() ? value.failed_phase() == flexedge::node::v2::APPLY_PHASE_UNSPECIFIED &&
                                 value.error_code().empty() && value.error().empty()
                           : valid_apply_phase(value.failed_phase()) && !value.error_code().empty() &&
                                 !value.error().empty();
}

}  // namespace service::node_channel
