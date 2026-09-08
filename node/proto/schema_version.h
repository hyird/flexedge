#pragma once

#include <cstdint>

namespace flexedge::node {

inline constexpr std::uint32_t kNodeSpecSchemaVersion{2};
// v4 adds route conditions, regex/suffix matching and 307/308 redirects.
// Old nodes must reject rather than silently ignore matching conditions.
inline constexpr std::uint32_t kClusterReleaseSchemaVersion{4};
inline constexpr bool supportedClusterReleaseSchema(std::uint32_t version) noexcept {
    return version >= 2 && version <= kClusterReleaseSchemaVersion;
}

} // namespace flexedge::node
