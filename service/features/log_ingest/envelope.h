#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "node/proto/edge_control.pb.h"
#include "node/proto/log_contract.h"
#include "service/common/http.h"
#include "service/features/log_ingest/log_envelope.pb.h"

namespace service::log_ingest {

inline bool bounded(std::string_view value, std::size_t maximum) { return value.size() <= maximum; }

inline bool uuid(std::string_view value) { return service::common::parseUuid(value).has_value(); }

inline bool agentId(std::string_view value) {
    return value.size() == 32 && std::ranges::all_of(value, [](unsigned char ch) {
               return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
           });
}

inline bool validLogLevel(std::string_view value) {
    return value == "info" || value == "warning" || value == "error";
}

inline bool validAccess(const flexedge::node::v2::AccessLog& value) {
    return uuid(value.id()) && uuid(value.website_id()) && value.occurred_unix_ms() > 0 &&
           bounded(value.client_ip(), flexedge::node::log_contract::kMaxClientIpBytes) &&
           !value.protocol().empty() &&
           bounded(value.protocol(), flexedge::node::log_contract::kMaxProtocolBytes) &&
           !value.method().empty() &&
           bounded(value.method(), flexedge::node::log_contract::kMaxMethodBytes) &&
           !value.host().empty() &&
           bounded(value.host(), flexedge::node::log_contract::kMaxHostBytes) &&
           !value.target().empty() &&
           bounded(value.target(), flexedge::node::log_contract::kMaxTargetBytes) &&
           value.status_code() >= 100 && value.status_code() <= 999 &&
           value.response_bytes() <= flexedge::node::log_contract::kMaxResponseBytes &&
           value.duration_ms() <= flexedge::node::log_contract::kMaxDurationMs &&
           bounded(value.user_agent(), flexedge::node::log_contract::kMaxUserAgentBytes) &&
           bounded(value.referer(), flexedge::node::log_contract::kMaxRefererBytes) &&
           bounded(value.request_headers(),
                   flexedge::node::log_contract::kMaxRequestHeadersBytes) &&
           bounded(value.request_body(), flexedge::node::log_contract::kMaxRequestBodyBytes) &&
           bounded(value.tls_fingerprint(),
                   flexedge::node::log_contract::kMaxTlsFingerprintBytes) &&
           bounded(value.response_headers(),
                   flexedge::node::log_contract::kMaxResponseHeadersBytes) &&
           bounded(value.query_string(), flexedge::node::log_contract::kMaxQueryStringBytes) &&
           bounded(value.cookies(), flexedge::node::log_contract::kMaxCookiesBytes);
}

inline bool validNodeLog(const flexedge::node::v2::NodeLog& value) {
    return uuid(value.id()) && value.occurred_unix_ms() > 0 && validLogLevel(value.level()) &&
           !value.category().empty() &&
           bounded(value.category(), flexedge::node::log_contract::kMaxNodeCategoryBytes) &&
           bounded(value.message(), flexedge::node::log_contract::kMaxNodeMessageBytes) &&
           !value.message().empty();
}

inline bool validEvent(const flexedge::node::v2::LogEvent& value) {
    if (value.has_access_log() == value.has_node_log()) {
        return false;
    }
    return value.has_access_log() ? validAccess(value.access_log())
                                  : validNodeLog(value.node_log());
}

inline bool validDelivery(const flexedge::node::v2::LogDelivery& value, std::string_view nodeId) {
    if (!uuid(nodeId) || value.node_id() != nodeId || value.events().empty() ||
        value.events_size() > static_cast<int>(flexedge::node::log_contract::kMaxDeliveryEvents) ||
        value.ByteSizeLong() > flexedge::node::log_contract::kMaxDeliveryBytes) {
        return false;
    }
    return std::ranges::all_of(value.events(), validEvent);
}

inline bool validEnvelope(const v2::QueuedLogDelivery& value) {
    return uuid(value.tenant_id()) && agentId(value.agent_id()) && value.has_delivery() &&
           validDelivery(value.delivery(), value.delivery().node_id());
}

inline std::string serializeEnvelope(std::string_view tenantId, std::string_view agentId,
                                     const flexedge::node::v2::LogDelivery& delivery) {
    v2::QueuedLogDelivery envelope;
    envelope.set_tenant_id(tenantId);
    envelope.set_agent_id(agentId);
    *envelope.mutable_delivery() = delivery;
    if (!validEnvelope(envelope)) {
        throw std::invalid_argument("invalid queued log delivery");
    }
    std::string bytes;
    if (!envelope.SerializeToString(&bytes)) {
        throw std::runtime_error("could not serialize queued log delivery");
    }
    return bytes;
}

inline std::optional<v2::QueuedLogDelivery> parseEnvelope(std::string_view bytes) {
    if (bytes.empty() ||
        bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return std::nullopt;
    }
    v2::QueuedLogDelivery envelope;
    if (!envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())) ||
        !validEnvelope(envelope)) {
        return std::nullopt;
    }
    return envelope;
}

} // namespace service::log_ingest
