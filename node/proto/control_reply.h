#pragma once
#include <optional>
#include <string_view>
#include <vector>
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {
inline bool matchesApplyResultAck(const v2::ServerEnvelope& reply, std::string_view requestId,
                                  const v2::ApplyResult& result) {
    if (!reply.has_apply_result_ack() || reply.request_id() != requestId)
        return false;
    const auto& ack = reply.apply_result_ack();
    return ack.node_id() == result.node_id() &&
           ack.node_spec_revision() == result.node_spec_revision() &&
           ack.release_id() == result.release_id() &&
           ack.manifest_digest() == result.manifest_digest() && ack.applied() == result.applied();
}

inline std::vector<v2::ServerEnvelope>
releaseProbeReplies(std::string_view requestId, std::string_view binaryDigest,
                    std::int64_t revision, std::string_view releaseId,
                    std::string_view manifestDigest, std::optional<v2::DesiredState> state) {
    std::vector<v2::ServerEnvelope> replies(1);
    replies.front().set_request_id(requestId);
    auto* ack = replies.front().mutable_release_probe_ack();
    ack->set_node_binary_sha256(binaryDigest);
    ack->set_desired_node_spec_revision(revision);
    ack->set_desired_release_id(releaseId);
    ack->set_desired_manifest_digest(manifestDigest);
    if (state) {
        // One snapshot supplies both frames, even if a newer release commits meanwhile.
        ack->set_desired_node_spec_revision(state->node_spec().content().revision());
        ack->set_desired_release_id(state->release().content().release_id());
        ack->set_desired_manifest_digest(state->release().digest_sha256());
        auto& body = replies.emplace_back();
        body.set_request_id(requestId);
        *body.mutable_desired_state() = std::move(*state);
    }
    return replies;
}
} // namespace flexedge::node
