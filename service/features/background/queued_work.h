#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace service::background {

// Work one write transaction queued for background workers. Passed by
// reference through every enqueue path; after the commit the committing
// worker notifies the changed tasks and wakes its own claim loops.
class queued_work final {
public:
    // The task (resource) whose state the transaction changed.
    void changed(std::string_view resource_id) {
        if (!resource_id.empty() && std::find(changed_.begin(), changed_.end(), resource_id) == changed_.end()) {
            changed_.emplace_back(resource_id);
        }
    }
    // Work of `work_type` (a static work_available type) the transaction made
    // claimable. The committing worker claims it first.
    void claim(std::string_view work_type) {
        if (std::find(claims_.begin(), claims_.end(), work_type) == claims_.end()) {
            claims_.push_back(work_type);
        }
    }
    [[nodiscard]] const std::vector<std::string_view>& claims() const noexcept {
        return claims_;
    }
    [[nodiscard]] const std::vector<std::string>& changed_tasks() const noexcept {
        return changed_;
    }

private:
    std::vector<std::string> changed_;
    std::vector<std::string_view> claims_;
};

}  // namespace service::background
