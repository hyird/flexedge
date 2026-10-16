#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

#include "service/features/node_release/release.h"

namespace service::node_release {

using snapshot = std::shared_ptr<const release>;

// Reads and digests the release files once, before any Worker starts. The
// release is immutable for the process lifetime: a new node binary is
// published by restarting the service, so a download or chunked transfer never
// observes a version switch and its descriptor paths stay valid.
inline snapshot load(const std::filesystem::path& binary_path, const std::filesystem::path& installer_path,
    const std::filesystem::path& manifest_path) {
    return std::make_shared<const release>(binary_path, installer_path, manifest_path);
}

// Each Worker's handle to the release loaded at startup (Ruvia worker state).
class catalog final {
public:
    explicit catalog(snapshot value)
        : release_(std::move(value)) {
        if (!release_) {
            throw std::invalid_argument("node release catalog requires a loaded release");
        }
    }

    [[nodiscard]] const snapshot& current() const noexcept {
        return release_;
    }

private:
    snapshot release_;
};

template <typename context_type>
const snapshot& current(context_type& context) {
    return context.template worker_state<catalog>().current();
}

}  // namespace service::node_release
