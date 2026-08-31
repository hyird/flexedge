#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "node/runtime/compiled_config.h"

namespace flexedge::node {

class RuntimeState final {
  public:
    void apply(std::shared_ptr<const v2::ActiveState> state,
               std::vector<v2::DeliveryObject> objects) {
        auto compiled =
            std::make_shared<const CompiledConfig>(std::move(state), std::move(objects));
        validateNext(*compiled);
        publish(std::move(compiled));
    }

    void validateNext(const CompiledConfig& config) const {
        const auto current = config_.load();
        if (!current) {
            return;
        }
        const auto specRevision = config.nodeSpec().content().revision();
        const auto currentSpecRevision = current->nodeSpec().content().revision();
        if (specRevision < currentSpecRevision ||
            (specRevision == currentSpecRevision &&
             config.nodeSpec().digest_sha256() != current->nodeSpec().digest_sha256())) {
            throw std::runtime_error("stale or conflicting node spec");
        }
        const auto releaseGeneration = config.release().content().generation();
        const auto currentReleaseGeneration = current->release().content().generation();
        if (releaseGeneration < currentReleaseGeneration ||
            (releaseGeneration == currentReleaseGeneration &&
             (config.release().content().release_id() !=
                  current->release().content().release_id() ||
              config.release().digest_sha256() != current->release().digest_sha256()))) {
            throw std::runtime_error("stale or conflicting cluster release");
        }
    }

    void publish(std::shared_ptr<const CompiledConfig> config) noexcept {
        config_.store(std::move(config));
    }

    [[nodiscard]] std::shared_ptr<const CompiledConfig> config() const noexcept {
        return config_.load();
    }

    [[nodiscard]] std::int64_t appliedNodeSpecRevision() const noexcept {
        const auto value = config();
        return value ? value->nodeSpec().content().revision() : 0;
    }

    [[nodiscard]] std::string activeReleaseId() const {
        const auto value = config();
        return value ? value->release().content().release_id() : std::string{};
    }

    [[nodiscard]] std::string activeManifestDigest() const {
        const auto value = config();
        return value ? value->release().digest_sha256() : std::string{};
    }

  private:
    std::atomic<std::shared_ptr<const CompiledConfig>> config_;
};

} // namespace flexedge::node
