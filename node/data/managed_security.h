#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace flexedge::node {

struct managed_security_settings final {
    std::uint32_t paranoia_level_{1};
    std::uint32_t inbound_threshold_{5};
    std::uint32_t body_limit_bytes_{1024 * 1024};
    bool detection_only_{};
    std::vector<std::uint32_t> excluded_rules_{};
    bool operator==(const managed_security_settings&) const = default;
};

struct managed_security_result final {
    std::uint16_t status_{};
    bool engine_error_{};
    std::optional<std::uint32_t> anomaly_score_{};
    std::vector<std::uint32_t> matched_rules_{};
};

// Opaque, frozen after construction. No RulesSet pointer, load or mutation API escapes.
// A request's transaction pins this object until all native request state is destroyed.
class managed_security final {
public:
    explicit managed_security(const managed_security_settings& settings);
    ~managed_security();
    managed_security(const managed_security&) = delete;
    managed_security& operator=(const managed_security&) = delete;

    [[nodiscard]] managed_security_result inspect(std::string_view client_ip,
        std::string_view method, std::string_view uri, bool secure,
        const std::vector<std::pair<std::string_view, std::string_view>>& headers,
        std::string_view body) const;

private:
    struct state;
    std::unique_ptr<state> state_;
};

}  // namespace flexedge::node
