#pragma once

#include <string>
#include <string_view>

#ifndef FLEXEDGE_NODE_VERSION
#error "Node version consumers must link flexedge_node_version"
#endif

namespace flexedge::node {

inline constexpr std::string_view kNodeVersion{FLEXEDGE_NODE_VERSION};

inline std::string nodeUserAgent() {
    std::string value{"FlexEdge-Node/"};
    value += kNodeVersion;
    return value;
}

} // namespace flexedge::node
