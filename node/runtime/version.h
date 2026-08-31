#pragma once

#include <string>
#include <string_view>

#ifndef FLEXEDGE_NODE_VERSION
#define FLEXEDGE_NODE_VERSION "0.3.20"
#endif

namespace flexedge::node {

inline constexpr std::string_view kNodeVersion{FLEXEDGE_NODE_VERSION};

inline std::string nodeUserAgent() {
    std::string value{"FlexEdge-Node/"};
    value += kNodeVersion;
    return value;
}

} // namespace flexedge::node
