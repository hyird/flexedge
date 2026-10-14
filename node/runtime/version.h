#pragma once

#include <string>
#include <string_view>

#ifndef FLEXEDGE_NODE_VERSION
#error "Node version consumers must link flexedge_node_version"
#endif

namespace flexedge::node {

inline constexpr std::string_view node_version{FLEXEDGE_NODE_VERSION};

inline std::string node_user_agent() {
    std::string value{"FlexEdge-Node/"};
    value += node_version;
    return value;
}

}  // namespace flexedge::node
