#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace flexedge::node {

inline constexpr std::string_view kControlSubprotocol{"flexedge.node.v2"};
inline constexpr std::uint32_t kReleaseProbeIntervalSeconds{1};
// Keep a serialized protobuf envelope well below the 4 MiB WebSocket message limit.
inline constexpr std::size_t kNodeReleaseChunkBytes{256 * 1024};
inline constexpr std::uint64_t kMaximumNodeReleaseBytes{512ULL * 1024 * 1024};

} // namespace flexedge::node
