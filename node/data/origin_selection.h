#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "node/data/origin_health.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

// Borrows the immutable Website's origin objects and id. The Website must outlive
// this plan, and both must outlive every cursor borrowing the published plan.
class origin_plan final {
public:
    origin_plan() = default;

    origin_plan(const v2::Website& website,
        std::string_view group,
        std::span<const std::string> ids = {}) {
        const auto target_group = group.empty() ? std::string_view{"default"} : group;
        for (const auto& origin : website.origins()) {
            if (!origin.enabled() || origin.weight() == 0) {
                continue;
            }
            const auto role = origin.role() == "primary" ? 0U : origin.role() == "backup" ? 1U
                                                                                          : 2U;
            if (role == roles_.size()) {
                continue;
            }
            const auto selected = ids.empty()
                                      ? (origin.group().empty() ? std::string_view{"default"}
                                                                : std::string_view(origin.group())) == target_group
                                      : std::ranges::find(ids, origin.id()) != ids.end();
            if (!selected) {
                continue;
            }
            auto& compiled = roles_[role];
            const auto total = compiled.empty() ? std::uint64_t{} : compiled.back().weight_end_;
            compiled.push_back({&origin, total + origin.weight(), {website.id(), origin.id()}});
        }
    }

    [[nodiscard]] bool empty() const noexcept {
        return roles_[0].empty() && roles_[1].empty();
    }

private:
    friend class origin_cursor;

    struct candidate final {
        const v2::Origin* origin_;
        std::uint64_t weight_end_;
        origin_health_key_view health_key_;
    };

    std::array<std::vector<candidate>, 2> roles_;
};

// Request-local iteration state; only the immutable health snapshot is owned.
// A null snapshot means all origins have unknown (therefore healthy) status.
class origin_cursor final {
public:
    origin_cursor() = default;

    origin_cursor(const origin_plan& plan,
        std::uint64_t sequence,
        std::shared_ptr<const origin_health_snapshot> health)
        : plan_(&plan),
          health_(std::move(health)) {
        for (std::size_t role = 0; role < plan.roles_.size(); ++role) {
            const auto& candidates = plan.roles_[role];
            if (!candidates.empty()) {
                const auto weight = sequence % candidates.back().weight_end_;
                pivots_[role] = static_cast<std::size_t>(
                    std::upper_bound(candidates.begin(), candidates.end(), weight,
                        [](std::uint64_t selected, const origin_plan::candidate& value) {
                            return selected < value.weight_end_;
                        }) -
                    candidates.begin());
            }
        }
        reset_role();
    }

    [[nodiscard]] const v2::Origin* next() noexcept {
        if (plan_ == nullptr) {
            return nullptr;
        }
        for (;;) {
            if (remaining_ == 0) {
                if (++role_ == plan_->roles_.size()) {
                    // A second traversal produces the unhealthy stable partition
                    // without storing request-local candidate lists or changing pivots.
                    if (unhealthy_pass_ || !saw_unhealthy_) {
                        plan_ = nullptr;
                        return nullptr;
                    }
                    unhealthy_pass_ = true;
                    role_ = 0;
                }
                reset_role();
                continue;
            }
            const auto& candidates = plan_->roles_[role_];
            const auto& candidate = candidates[index_];
            const auto* origin = candidate.origin_;
            --remaining_;
            if (++index_ == candidates.size()) {
                index_ = 0;
            }
            const auto healthy = !health_ || health_->healthy(candidate.health_key_);
            if (!healthy) {
                saw_unhealthy_ = true;
            }
            if (healthy != unhealthy_pass_) {
                return origin;
            }
        }
    }

private:
    void reset_role() noexcept {
        index_ = pivots_[role_];
        remaining_ = plan_->roles_[role_].size();
    }

    const origin_plan* plan_{};
    std::shared_ptr<const origin_health_snapshot> health_;
    std::array<std::size_t, 2> pivots_{};
    std::size_t role_{};
    std::size_t index_{};
    std::size_t remaining_{};
    bool unhealthy_pass_{};
    bool saw_unhealthy_{};
};

}  // namespace flexedge::node
