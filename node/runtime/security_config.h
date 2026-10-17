#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "node/data/managed_security.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/security_validation.h"

namespace flexedge::node {

// Built once before publication. Only the closed, frozen managed detector is shared;
// native expression programs (and their RE2 objects) still belong to each Worker.
class security_config final {
public:
    explicit security_config(const std::vector<const v2::Website*>& websites) {
        for (const auto* website : websites) {
            if (!website->has_security()) {
                continue;
            }
            const auto& policy = website->security();
            validate_security(policy);
            const auto identity = policy.id() + ":" + std::to_string(policy.revision());
            const auto source = policy.SerializeAsString();
            auto [found, inserted] = policies_.try_emplace(identity);
            if (!inserted && found->second.source_ != source) {
                throw std::invalid_argument("conflicting security policy revision");
            }
            if (inserted) {
                found->second.source_ = source;
                const auto& managed = policy.managed();
                if (managed.enabled()) {
                    managed_security_settings settings{
                        .paranoia_level_ = managed.paranoia_level(),
                        .inbound_threshold_ = managed.inbound_threshold(),
                        .body_limit_bytes_ = managed.body_limit_bytes(),
                        .detection_only_ = managed.detection_only()};
                    settings.excluded_rules_.assign(managed.excluded_rules().begin(), managed.excluded_rules().end());
                    found->second.managed_ = std::make_shared<const managed_security>(settings);
                }
            }
            websites_.emplace(website->id(), &found->second);
        }
    }
    security_config(const security_config&) = delete;
    security_config& operator=(const security_config&) = delete;
    security_config(security_config&&) = delete;
    security_config& operator=(security_config&&) = delete;
    [[nodiscard]] const managed_security* managed(std::string_view website) const {
        const auto found = websites_.find(std::string(website));
        return found == websites_.end() ? nullptr : found->second->managed_.get();
    }

private:
    struct policy final {
        std::string source_{};
        std::shared_ptr<const managed_security> managed_{};
    };
    std::unordered_map<std::string, policy> policies_;
    std::unordered_map<std::string, const policy*> websites_;
};

}  // namespace flexedge::node
