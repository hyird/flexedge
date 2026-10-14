#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline std::vector<const v2::Origin*> origin_candidates(const v2::Website& website,
    std::uint64_t sequence,
    const v2::RouteRule* route = nullptr) {
    std::vector<const v2::Origin*> result;
    const auto target_group =
        route != nullptr && !route->origin_group().empty() ? std::string_view(route->origin_group())
        : website.default_origin_group().empty()           ? std::string_view{"default"}
                                                           : std::string_view(website.default_origin_group());
    auto append_role = [&](std::string_view role) {
        std::vector<const v2::Origin*> group;
        std::uint64_t total_weight{};
        for (const auto& origin : website.origins()) {
            const auto selected_by_legacy_route = route != nullptr && !route->origin_ids().empty();
            const auto selected =
                selected_by_legacy_route
                    ? std::ranges::find(route->origin_ids(), origin.id()) !=
                          route->origin_ids().end()
                    : (origin.group().empty() ? std::string_view{"default"}
                                              : std::string_view(origin.group())) == target_group;
            if (selected && origin.enabled() && origin.role() == role && origin.weight() > 0) {
                group.push_back(&origin);
                total_weight += origin.weight();
            }
        }
        if (group.empty()) {
            return;
        }
        auto selected_weight = sequence % total_weight;
        std::size_t selected{};
        for (; selected + 1 < group.size(); ++selected) {
            if (selected_weight < group[selected]->weight()) {
                break;
            }
            selected_weight -= group[selected]->weight();
        }
        for (std::size_t offset = 0; offset < group.size(); ++offset) {
            result.push_back(group[(selected + offset) % group.size()]);
        }
    };
    append_role("primary");
    append_role("backup");
    return result;
}

}  // namespace flexedge::node
