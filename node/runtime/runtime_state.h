#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "node/runtime/compiled_config.h"

namespace flexedge::node {

// RE2 maintains matching caches even behind its const API. Compile those objects on each
// data worker; immutable configuration shared between loops contains only their source.
inline std::shared_ptr<const route_patterns> compile_route_patterns(const compiled_config& config) {
    auto patterns = std::make_shared<route_patterns>();
    for (const auto* website : config.websites()) {
        validate_route_rules(*website, patterns.get());
    }
    return patterns;
}

class runtime_state final {
public:
    void validate_next(const compiled_config& config) const {
        const auto current = config_.load(std::memory_order_acquire);
        if (!current) {
            return;
        }
        const auto spec_revision = config.node_spec().revision();
        const auto current_spec_revision = current->node_spec().revision();
        if (spec_revision < current_spec_revision ||
            (spec_revision == current_spec_revision &&
                config.node_spec_digest() != current->node_spec_digest())) {
            throw std::runtime_error("stale or conflicting node spec");
        }
        const auto release_generation = config.release().generation();
        const auto current_release_generation = current->release().generation();
        if (release_generation < current_release_generation ||
            (release_generation == current_release_generation &&
                (config.release().release_id() != current->release().release_id() ||
                    config.release_digest() != current->release_digest()))) {
            throw std::runtime_error("stale or conflicting cluster release");
        }
    }

    void publish(std::shared_ptr<const compiled_config> config, std::shared_ptr<const route_patterns> patterns) noexcept {
        patterns_ = std::move(patterns);
        config_.store(std::move(config), std::memory_order_release);
    }

    // Owner loop only. A session pins its worker's patterns alongside its configuration.
    [[nodiscard]] std::shared_ptr<const route_patterns> patterns() const noexcept {
        return patterns_;
    }

    [[nodiscard]] std::shared_ptr<const compiled_config> config() const noexcept {
        return config_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::int64_t applied_node_spec_revision() const noexcept {
        const auto value = config();
        return value ? value->node_spec().revision() : 0;
    }

    [[nodiscard]] std::string active_release_id() const {
        const auto value = config();
        return value ? value->release().release_id() : std::string{};
    }

    [[nodiscard]] std::string active_manifest_digest() const {
        const auto value = config();
        return value ? value->release_digest() : std::string{};
    }

private:
    std::atomic<std::shared_ptr<const compiled_config>> config_;
    std::shared_ptr<const route_patterns> patterns_;
};

}  // namespace flexedge::node
