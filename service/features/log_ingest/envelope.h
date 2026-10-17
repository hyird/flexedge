#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/credential_validation.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/log_contract.h"
#include "service/common/uuid.h"
#include "service/features/log_ingest/log_envelope.pb.h"

namespace service::log_ingest {

inline bool bounded(std::string_view value, std::size_t maximum) {
    return value.size() <= maximum;
}

inline bool uuid(std::string_view value) {
    return service::common::parse_uuid(value).has_value();
}

inline bool valid_log_level(std::string_view value) {
    return value == "info" || value == "warning" || value == "error";
}

inline bool valid_access(const flexedge::node::v2::AccessLog& value) {
    return uuid(value.id()) && uuid(value.website_id()) && value.occurred_unix_ms() > 0 &&
           bounded(value.client_ip(), flexedge::node::log_contract::max_client_ip_bytes) &&
           !value.protocol().empty() &&
           bounded(value.protocol(), flexedge::node::log_contract::max_protocol_bytes) &&
           !value.method().empty() &&
           bounded(value.method(), flexedge::node::log_contract::max_method_bytes) &&
           !value.host().empty() &&
           bounded(value.host(), flexedge::node::log_contract::max_host_bytes) &&
           !value.target().empty() &&
           bounded(value.target(), flexedge::node::log_contract::max_target_bytes) &&
           value.status_code() >= 100 && value.status_code() <= 999 &&
           value.request_bytes() <= flexedge::node::log_contract::max_request_bytes &&
           value.response_bytes() <= flexedge::node::log_contract::max_response_bytes &&
           value.duration_ms() <= flexedge::node::log_contract::max_duration_ms &&
           bounded(value.user_agent(), flexedge::node::log_contract::max_user_agent_bytes) &&
           bounded(value.referer(), flexedge::node::log_contract::max_referer_bytes) &&
           bounded(value.request_headers(),
               flexedge::node::log_contract::max_request_headers_bytes) &&
           bounded(value.request_body(), flexedge::node::log_contract::max_request_body_bytes) &&
           bounded(value.tls_fingerprint(),
               flexedge::node::log_contract::max_tls_fingerprint_bytes) &&
           bounded(value.response_headers(),
               flexedge::node::log_contract::max_response_headers_bytes) &&
           bounded(value.query_string(), flexedge::node::log_contract::max_query_string_bytes) &&
           bounded(value.cookies(), flexedge::node::log_contract::max_cookies_bytes) &&
           (value.cache_status().empty() || value.cache_status() == "hit_memory" || value.cache_status() == "hit_disk" || value.cache_status() == "miss" || value.cache_status() == "bypass");
}

inline bool valid_node_log(const flexedge::node::v2::NodeLog& value) {
    return uuid(value.id()) && value.occurred_unix_ms() > 0 && valid_log_level(value.level()) &&
           !value.category().empty() &&
           bounded(value.category(), flexedge::node::log_contract::max_node_category_bytes) &&
           bounded(value.message(), flexedge::node::log_contract::max_node_message_bytes) &&
           !value.message().empty();
}

inline bool valid_event(const flexedge::node::v2::LogEvent& value) {
    if (value.has_access_log() == value.has_node_log()) {
        return false;
    }
    return value.has_access_log() ? valid_access(value.access_log())
                                  : valid_node_log(value.node_log());
}

inline bool valid_delivery(const flexedge::node::v2::LogDelivery& value, std::string_view node_id) {
    if (!uuid(node_id) || value.node_id() != node_id || value.events().empty() ||
        value.events_size() > static_cast<int>(flexedge::node::log_contract::max_delivery_events) ||
        value.ByteSizeLong() > flexedge::node::log_contract::max_delivery_bytes) {
        return false;
    }
    return std::ranges::all_of(value.events(), valid_event);
}

inline bool valid_envelope(const v2::QueuedLogDelivery& value) {
    return uuid(value.tenant_id()) && flexedge::node::valid_credential_node_id(value.agent_id()) &&
           value.has_delivery() &&
           valid_delivery(value.delivery(), value.delivery().node_id());
}

inline std::string serialize_envelope(std::string_view tenant_id, std::string_view agent_id,
    const flexedge::node::v2::LogDelivery& delivery) {
    v2::QueuedLogDelivery envelope;
    envelope.set_tenant_id(tenant_id);
    envelope.set_agent_id(agent_id);
    *envelope.mutable_delivery() = delivery;
    if (!valid_envelope(envelope)) {
        throw std::invalid_argument("invalid queued log delivery");
    }
    std::string bytes;
    if (!envelope.SerializeToString(&bytes)) {
        throw std::runtime_error("could not serialize queued log delivery");
    }
    return bytes;
}

inline std::optional<v2::QueuedLogDelivery> parse_envelope(std::string_view bytes) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    v2::QueuedLogDelivery envelope;
    if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) ||
        !valid_envelope(envelope)) {
        return std::nullopt;
    }
    return envelope;
}

}  // namespace service::log_ingest
