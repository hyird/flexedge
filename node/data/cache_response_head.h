#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace flexedge::node {

// Cache metadata describes the delivered representation, without transfer framing.
struct cache_response_head final {
    std::uint16_t status_{502};
    std::vector<std::pair<std::string, std::string>> headers_{};
};

}  // namespace flexedge::node
