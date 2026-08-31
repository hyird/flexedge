#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline std::vector<const v2::Origin*> originCandidates(const v2::Website& website,
                                                       std::uint64_t sequence,
                                                       const v2::RouteRule* route = nullptr) {
    std::vector<const v2::Origin*> result;
    const auto targetGroup =
        route != nullptr && !route->origin_group().empty() ? std::string_view(route->origin_group())
        : website.default_origin_group().empty()           ? std::string_view{"default"}
                                                 : std::string_view(website.default_origin_group());
    auto appendRole = [&](std::string_view role) {
        std::vector<const v2::Origin*> group;
        std::uint64_t totalWeight{};
        for (const auto& origin : website.origins()) {
            const auto selectedByLegacyRoute = route != nullptr && !route->origin_ids().empty();
            const auto selected =
                selectedByLegacyRoute
                    ? std::ranges::find(route->origin_ids(), origin.id()) !=
                          route->origin_ids().end()
                    : (origin.group().empty() ? std::string_view{"default"}
                                              : std::string_view(origin.group())) == targetGroup;
            if (selected && origin.enabled() && origin.role() == role && origin.weight() > 0) {
                group.push_back(&origin);
                totalWeight += origin.weight();
            }
        }
        if (group.empty()) {
            return;
        }
        auto selectedWeight = sequence % totalWeight;
        std::size_t selected{};
        for (; selected + 1 < group.size(); ++selected) {
            if (selectedWeight < group[selected]->weight()) {
                break;
            }
            selectedWeight -= group[selected]->weight();
        }
        for (std::size_t offset = 0; offset < group.size(); ++offset) {
            result.push_back(group[(selected + offset) % group.size()]);
        }
    };
    appendRole("primary");
    appendRole("backup");
    return result;
}

} // namespace flexedge::node
