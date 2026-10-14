#pragma once

#include <cstdint>

namespace flexedge::node {

inline constexpr std::uint32_t node_spec_schema_version{2};
// v6 separates redirects, immutable-input rewrites, and cumulative origin rules.
// Old nodes must reject rather than execute a different rule for the same request.
inline constexpr std::uint32_t cluster_release_schema_version{6};
inline constexpr bool supported_cluster_release_schema(std::uint32_t version) noexcept {
    return version >= 2 && version <= cluster_release_schema_version;
}

}  // namespace flexedge::node
