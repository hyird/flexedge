#pragma once
#include <optional>
#include <string_view>
#include <vector>

#include "common/sha256.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {
inline bool valid_welcome(const v2::Welcome& value) {
    return !value.node_id().empty() && value.desired_node_spec_revision() > 0 &&
           !value.desired_release_id().empty() &&
           flexedge::crypto::is_sha256_digest(value.desired_manifest_digest()) &&
           flexedge::crypto::is_sha256_digest(value.node_binary_sha256());
}

inline bool matches_release_probe_ack(const v2::ServerEnvelope& reply, std::string_view request_id) {
    if (reply.request_id() != request_id || !reply.has_release_probe_ack()) {
        return false;
    }
    const auto& value = reply.release_probe_ack();
    return flexedge::crypto::is_sha256_digest(value.node_binary_sha256()) &&
           value.desired_node_spec_revision() > 0 && !value.desired_release_id().empty() &&
           flexedge::crypto::is_sha256_digest(value.desired_manifest_digest());
}

inline bool matches_heartbeat_ack(const v2::ServerEnvelope& reply, std::string_view request_id) {
    return reply.request_id() == request_id && reply.has_heartbeat_ack() &&
           flexedge::crypto::is_sha256_digest(reply.heartbeat_ack().node_binary_sha256());
}

inline bool matches_apply_result_ack(const v2::ServerEnvelope& reply, std::string_view request_id,
    const v2::ApplyResult& result) {
    if (!reply.has_apply_result_ack() || reply.request_id() != request_id) {
        return false;
    }
    const auto& ack = reply.apply_result_ack();
    return ack.node_id() == result.node_id() &&
           ack.node_spec_revision() == result.node_spec_revision() &&
           ack.release_id() == result.release_id() &&
           ack.manifest_digest() == result.manifest_digest() && ack.applied() == result.applied();
}

inline std::vector<v2::ServerEnvelope>
release_probe_replies(std::string_view request_id, std::string_view binary_digest,
    std::int64_t revision, std::string_view release_id,
    std::string_view manifest_digest, std::optional<v2::DesiredState> state) {
    std::vector<v2::ServerEnvelope> replies(1);
    replies.front().set_request_id(request_id);
    auto* ack = replies.front().mutable_release_probe_ack();
    ack->set_node_binary_sha256(binary_digest);
    ack->set_desired_node_spec_revision(revision);
    ack->set_desired_release_id(release_id);
    ack->set_desired_manifest_digest(manifest_digest);
    if (state) {
        // One snapshot supplies both frames, even if a newer release commits meanwhile.
        ack->set_desired_node_spec_revision(state->node_spec().content().revision());
        ack->set_desired_release_id(state->release().content().release_id());
        ack->set_desired_manifest_digest(state->release().digest_sha256());
        auto& body = replies.emplace_back();
        body.set_request_id(request_id);
        *body.mutable_desired_state() = std::move(*state);
    }
    return replies;
}
}  // namespace flexedge::node
