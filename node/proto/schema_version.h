#pragma once

#include <cstdint>

namespace flexedge::node {

inline constexpr std::uint32_t kNodeSpecSchemaVersion{2};
// v6 separates redirects, immutable-input rewrites, and cumulative origin rules.
// Old nodes must reject rather than execute a different rule for the same request.
inline constexpr std::uint32_t kClusterReleaseSchemaVersion{6};
inline constexpr bool supportedClusterReleaseSchema(std::uint32_t version) noexcept {
    return version >= 2 && version <= kClusterReleaseSchemaVersion;
}

} // namespace flexedge::node
