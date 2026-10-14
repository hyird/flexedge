#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace flexedge::node {

inline constexpr std::string_view control_subprotocol{"flexedge.node.v2"};
inline constexpr std::uint32_t release_watch_maximum_seconds{10};
// Keep a serialized protobuf envelope well below the 4 MiB WebSocket message limit.
inline constexpr std::size_t node_release_chunk_bytes{256 * 1024};
inline constexpr std::uint64_t maximum_node_release_bytes{512ULL * 1024 * 1024};

}  // namespace flexedge::node
