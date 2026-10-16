#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "node/data/buffered_bytes_budget.h"

namespace flexedge::node {

// A complete origin response held in memory for response compression. Headers exclude
// hop-by-hop and framing fields; the body is decoded from its transfer coding. The
// reservation covers every byte the response retains.
struct buffered_proxy_response final {
    buffered_bytes_lease reservation_;
    std::uint16_t status_{502};
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

}  // namespace flexedge::node
