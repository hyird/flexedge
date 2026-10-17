#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "node/data/route_program.h"
#include "node/data/security_program.h"

namespace flexedge::node {

// Construct and retain this view on one data worker. Its program may own RE2 caches;
// only the source configuration crosses loops.
class worker_execution_view final {
public:
    explicit worker_execution_view(std::shared_ptr<const compiled_config> source)
        : source_(source ? std::move(source)
                         : throw std::invalid_argument("worker execution source cannot be empty")),
          program_(*source_),
          security_(*source_) {}

    [[nodiscard]] const compiled_config& config() const noexcept {
        return *source_;
    }

    [[nodiscard]] const route_program& routes() const noexcept {
        return program_;
    }
    [[nodiscard]] const security_program& security() const noexcept {
        return security_;
    }

    [[nodiscard]] const std::shared_ptr<const compiled_config>& source() const noexcept {
        return source_;
    }

private:
    std::shared_ptr<const compiled_config> source_;
    route_program program_;
    security_program security_;
};

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

    // Control-loop publication never creates or retains worker execution resources.
    void publish_source(std::shared_ptr<const compiled_config> config) noexcept {
        config_.store(std::move(config), std::memory_order_release);
    }

    // Owner loop only. Publication and request acquisition always use the whole pair.
    void publish_execution(std::shared_ptr<const worker_execution_view> execution) noexcept {
        execution_ = std::move(execution);
        publish_source(execution_ ? execution_->source() : nullptr);
    }

    [[nodiscard]] std::shared_ptr<const worker_execution_view> execution() const noexcept {
        return execution_;
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
    std::shared_ptr<const worker_execution_view> execution_;
};

}  // namespace flexedge::node
