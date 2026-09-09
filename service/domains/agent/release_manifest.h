#pragma once

#include <optional>
#include <string_view>
#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"

namespace service::agent {

inline std::optional<flexedge::node::v2::ClusterReleaseManifest> parseReleaseManifest(
    std::string_view plaintext, std::string_view expectedDigest,
    std::string_view releaseId, std::string_view clusterId) {
    flexedge::node::v2::ClusterReleaseManifest manifest;
    if (!flexedge::node::parseArtifact(plaintext, manifest) ||
        manifest.digest_sha256() != expectedDigest ||
        flexedge::node::artifactDigest(manifest.content()) != manifest.digest_sha256() ||
        manifest.content().release_id() != releaseId ||
        manifest.content().cluster_id() != clusterId) {
        return std::nullopt;
    }
    return manifest;
}

} // namespace service::agent