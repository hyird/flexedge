#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "node/runtime/compiled_config.h"

namespace flexedge::node {

class runtime_state final {
public:
    void validate_next(const compiled_config& config) const {
        const auto current = config_.load(std::memory_order_acquire);
        if (!current) {
            return;
        }
        const auto spec_revision = config.node_spec().content().revision();
        const auto current_spec_revision = current->node_spec().content().revision();
        if (spec_revision < current_spec_revision ||
            (spec_revision == current_spec_revision &&
                config.node_spec().digest_sha256() != current->node_spec().digest_sha256())) {
            throw std::runtime_error("stale or conflicting node spec");
        }
        const auto release_generation = config.release().content().generation();
        const auto current_release_generation = current->release().content().generation();
        if (release_generation < current_release_generation ||
            (release_generation == current_release_generation &&
                (config.release().content().release_id() !=
                        current->release().content().release_id() ||
                    config.release().digest_sha256() != current->release().digest_sha256()))) {
            throw std::runtime_error("stale or conflicting cluster release");
        }
    }

    void publish(std::shared_ptr<const compiled_config> config) noexcept {
        config_.store(std::move(config), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const compiled_config> config() const noexcept {
        return config_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::int64_t applied_node_spec_revision() const noexcept {
        const auto value = config();
        return value ? value->node_spec().content().revision() : 0;
    }

    [[nodiscard]] std::string active_release_id() const {
        const auto value = config();
        return value ? value->release().content().release_id() : std::string{};
    }

    [[nodiscard]] std::string active_manifest_digest() const {
        const auto value = config();
        return value ? value->release().digest_sha256() : std::string{};
    }

private:
    std::atomic<std::shared_ptr<const compiled_config>> config_;
};

}  // namespace flexedge::node
