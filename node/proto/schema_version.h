#pragma once

#include <cstdint>

namespace flexedge::node {

inline constexpr std::uint32_t kNodeSpecSchemaVersion{2};
// v3 adds hostname filters and explicit path/query processing. Old nodes must
// reject rather than silently execute these rules with v2 semantics.
inline constexpr std::uint32_t kClusterReleaseSchemaVersion{3};

} // namespace flexedge::node
