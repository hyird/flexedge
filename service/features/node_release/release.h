#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include "service/features/node_release/artifact.h"
#include "service/features/node_release/manifest.h"

namespace service::node_release {

class IncompleteRelease final : public std::runtime_error {
  public:
    IncompleteRelease() : std::runtime_error("node release files are not yet consistent") {}
};

class Release final {
  public:
    Release(const std::filesystem::path& binaryPath, const std::filesystem::path& installerPath,
            const std::filesystem::path& manifestPath)
        : binary_(binaryPath), installer_(installerPath), manifest_(manifestPath) {
        const auto metadata = parseManifest(manifest_.contents(256));
        if (!metadata) {
            throw std::runtime_error("node release manifest is invalid");
        }
        if (metadata->digest != binary_.digest()) {
            throw IncompleteRelease{};
        }
        version_ = metadata->version;
    }

    [[nodiscard]] const Artifact& binary() const noexcept { return binary_; }
    [[nodiscard]] const Artifact& installer() const noexcept { return installer_; }
    [[nodiscard]] const std::string& version() const noexcept { return version_; }

    [[nodiscard]] bool matchesSources(const std::filesystem::path& binaryPath,
                                      const std::filesystem::path& installerPath,
                                      const std::filesystem::path& manifestPath) const noexcept {
        return binary_.matchesSource(binaryPath) && installer_.matchesSource(installerPath) &&
               manifest_.matchesSource(manifestPath);
    }

  private:
    Artifact binary_;
    Artifact installer_;
    Artifact manifest_;
    std::string version_;
};

} // namespace service::node_release
