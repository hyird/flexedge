#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>

#include "service/features/node_release/artifact.h"
#include "service/features/node_release/manifest.h"

namespace service::node_release {

class incomplete_release final : public std::runtime_error {
public:
    incomplete_release()
        : std::runtime_error("node release files are not yet consistent") {}
};

class release final {
public:
    release(const std::filesystem::path& binary_path, const std::filesystem::path& installer_path,
        const std::filesystem::path& manifest_path)
        : binary_(binary_path),
          installer_(installer_path),
          manifest_(manifest_path) {
        const auto metadata = parse_manifest(manifest_.contents(256));
        if (!metadata) {
            throw std::runtime_error("node release manifest is invalid");
        }
        if (metadata->digest_ != binary_.digest()) {
            throw incomplete_release{};
        }
        version_ = metadata->version_;
    }

    [[nodiscard]] const artifact& binary() const noexcept {
        return binary_;
    }
    [[nodiscard]] const artifact& installer() const noexcept {
        return installer_;
    }
    [[nodiscard]] const std::string& version() const noexcept {
        return version_;
    }

    [[nodiscard]] bool matches_sources(const std::filesystem::path& binary_path,
        const std::filesystem::path& installer_path,
        const std::filesystem::path& manifest_path) const noexcept {
        return binary_.matches_source(binary_path) && installer_.matches_source(installer_path) &&
               manifest_.matches_source(manifest_path);
    }

private:
    artifact binary_;
    artifact installer_;
    artifact manifest_;
    std::string version_;
};

}  // namespace service::node_release
