#pragma once

#include <cstdint>
#include <string_view>

namespace flexedge::node {

inline constexpr std::string_view kControlSubprotocol{"flexedge.node.v2"};
inline constexpr std::uint32_t kReleaseProbeIntervalSeconds{1};

} // namespace flexedge::node
