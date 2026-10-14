#pragma once

#include <optional>
#include <string_view>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"

namespace service::agent {

inline std::optional<flexedge::node::v2::ClusterReleaseManifest> parse_release_manifest(
    std::string_view plaintext, std::string_view expected_digest,
    std::string_view release_id, std::string_view cluster_id) {
    flexedge::node::v2::ClusterReleaseManifest manifest;
    if (!flexedge::node::parse_artifact(plaintext, manifest) ||
        manifest.digest_sha256() != expected_digest ||
        flexedge::node::artifact_digest(manifest.content()) != manifest.digest_sha256() ||
        manifest.content().release_id() != release_id ||
        manifest.content().cluster_id() != cluster_id) {
        return std::nullopt;
    }
    return manifest;
}

}  // namespace service::agent
