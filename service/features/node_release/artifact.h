#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "node/runtime/binary_digest.h"
#include "node/runtime/release_response.h"

namespace service::node_release {

class IncompleteRelease final : public std::runtime_error {
  public:
    IncompleteRelease() : std::runtime_error("node release files are not yet consistent") {}
};

struct Manifest final {
    std::string version;
    std::string digest;
};

inline std::optional<Manifest> parseManifest(std::string_view value) {
    constexpr std::string_view versionPrefix{"version="};
    constexpr std::string_view digestPrefix{"sha256="};
    const auto takeLine = [&value]() -> std::optional<std::string_view> {
        const auto lineEnd = value.find('\n');
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }
        auto line = value.substr(0, lineEnd);
        value.remove_prefix(lineEnd + 1);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        return line;
    };

    const auto headerLine = takeLine();
    const auto versionLine = takeLine();
    const auto digestLine = takeLine();
    if (!headerLine || *headerLine != "flexedge-node-release-v1" || !versionLine || !digestLine ||
        !value.empty()) {
        return std::nullopt;
    }
    if (!versionLine->starts_with(versionPrefix)) {
        return std::nullopt;
    }
    if (!digestLine->starts_with(digestPrefix)) {
        return std::nullopt;
    }

    Manifest result{.version = std::string(versionLine->substr(versionPrefix.size())),
                    .digest = std::string(digestLine->substr(digestPrefix.size()))};
    if (!flexedge::node::validNodeReleaseVersion(result.version) ||
        !flexedge::node::validNodeReleaseDigest(result.digest)) {
        return std::nullopt;
    }
    return result;
}

class Artifact final {
  public:
    explicit Artifact(const std::filesystem::path& sourcePath) {
#ifdef _WIN32
        path_ = sourcePath;
        std::error_code error;
        sourceSize_ = std::filesystem::file_size(sourcePath, error);
        if (error)
            throw std::runtime_error("node release artifact is unavailable");
        sourceModified_ = std::filesystem::last_write_time(sourcePath, error);
        if (error)
            throw std::runtime_error("node release artifact is unavailable");
#else
        descriptor_ = ::open(sourcePath.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor_ < 0) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        struct stat metadata{};
        if (::fstat(descriptor_, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("node release artifact is not a regular file");
        }
        sourceStamp_ = stamp(metadata);
        path_ = "/proc/self/fd/" + std::to_string(descriptor_);
#endif
        try {
            digest_ = flexedge::node::binarySha256(path_);
            entityTag_ = flexedge::node::nodeReleaseEntityTag(digest_);
        } catch (...) {
#ifndef _WIN32
            ::close(descriptor_);
            descriptor_ = -1;
#endif
            throw;
        }
    }

    ~Artifact() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#endif
    }

    Artifact(const Artifact&) = delete;
    Artifact& operator=(const Artifact&) = delete;
    Artifact(Artifact&&) = delete;
    Artifact& operator=(Artifact&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& digest() const noexcept { return digest_; }
    [[nodiscard]] const std::string& entityTag() const noexcept { return entityTag_; }

    [[nodiscard]] std::string contents(std::size_t maximumBytes) const {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        std::string result;
        std::array<char, 256> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count > maximumBytes - result.size()) {
                throw std::runtime_error("node release artifact exceeds size limit");
            }
            result.append(buffer.data(), count);
        }
        if (!input.eof()) {
            throw std::runtime_error("could not read node release artifact");
        }
        return result;
    }

    [[nodiscard]] bool matchesSource(const std::filesystem::path& sourcePath) const noexcept {
#ifdef _WIN32
        std::error_code error;
        return std::filesystem::file_size(sourcePath, error) == sourceSize_ && !error &&
               std::filesystem::last_write_time(sourcePath, error) == sourceModified_ && !error;
#else
        struct stat metadata{};
        return ::lstat(sourcePath.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) &&
               stamp(metadata) == sourceStamp_;
#endif
    }

  private:
#ifndef _WIN32
    struct SourceStamp final {
        std::uint64_t device{};
        std::uint64_t inode{};
        std::uint64_t size{};
        std::int64_t modifiedSeconds{};
        std::int64_t modifiedNanoseconds{};

        bool operator==(const SourceStamp&) const = default;
    };

    [[nodiscard]] static SourceStamp stamp(const struct stat& metadata) noexcept {
#if defined(__APPLE__)
        return {.device = static_cast<std::uint64_t>(metadata.st_dev),
                .inode = static_cast<std::uint64_t>(metadata.st_ino),
                .size = static_cast<std::uint64_t>(metadata.st_size),
                .modifiedSeconds = metadata.st_mtimespec.tv_sec,
                .modifiedNanoseconds = metadata.st_mtimespec.tv_nsec};
#else
        return {.device = static_cast<std::uint64_t>(metadata.st_dev),
                .inode = static_cast<std::uint64_t>(metadata.st_ino),
                .size = static_cast<std::uint64_t>(metadata.st_size),
                .modifiedSeconds = metadata.st_mtim.tv_sec,
                .modifiedNanoseconds = metadata.st_mtim.tv_nsec};
#endif
    }

    int descriptor_{-1};
    SourceStamp sourceStamp_;
#else
    std::uintmax_t sourceSize_{};
    std::filesystem::file_time_type sourceModified_{};
#endif
    std::filesystem::path path_;
    std::string digest_;
    std::string entityTag_;
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

class Catalog final {
  public:
    using Snapshot = std::shared_ptr<const Release>;

    explicit Catalog(std::chrono::nanoseconds probeInterval = std::chrono::seconds(1),
                     std::chrono::nanoseconds retiredReleaseRetention = kRetiredReleaseRetention)
        : probeInterval_(probeInterval), retiredReleaseRetention_(retiredReleaseRetention) {}

    void configure(const std::filesystem::path& binaryPath,
                   const std::filesystem::path& installerPath,
                   const std::filesystem::path& manifestPath) {
        auto release = std::make_shared<const Release>(binaryPath, installerPath, manifestPath);
        Snapshot expected;
        if (!release_.compare_exchange_strong(expected, std::move(release),
                                              std::memory_order_release,
                                              std::memory_order_acquire)) {
            throw std::logic_error("node release is already configured");
        }
        binaryPath_ = binaryPath;
        installerPath_ = installerPath;
        manifestPath_ = manifestPath;
    }

    [[nodiscard]] Snapshot current() {
        auto release = release_.load(std::memory_order_acquire);
        if (!release) {
            throw std::logic_error("node release is not configured");
        }
        refreshIfDue(release);
        return release_.load(std::memory_order_acquire);
    }

  private:
    static constexpr auto kRetiredReleaseRetention = std::chrono::minutes(2);

    struct RetiredRelease final {
        Snapshot snapshot;
        std::int64_t expiresAtNs{};
    };

    [[nodiscard]] static std::int64_t monotonicNowNs() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] bool claimRefresh() noexcept {
        const auto now = monotonicNowNs();
        auto due = nextProbeNs_.load(std::memory_order_relaxed);
        while (now >= due) {
            const auto next = now + probeInterval_.count();
            if (nextProbeNs_.compare_exchange_weak(due, next, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void refreshIfDue(const Snapshot& release) noexcept {
        if (!claimRefresh() || refreshing_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        const RefreshGuard guard(refreshing_);
        const auto now = monotonicNowNs();
        std::erase_if(retiredReleases_,
                      [now](const RetiredRelease& retired) { return retired.expiresAtNs <= now; });
        if (release->matchesSources(binaryPath_, installerPath_, manifestPath_)) {
            return;
        }
        try {
            auto replacement =
                std::make_shared<const Release>(binaryPath_, installerPath_, manifestPath_);
            // c.file() reopens this descriptor path after its handler returns. Retaining prior
            // releases through the response write timeout keeps an in-flight download stable.
            retiredReleases_.push_back(
                {.snapshot = release, .expiresAtNs = now + retiredReleaseRetention_.count()});
            release_.store(std::move(replacement), std::memory_order_release);
        } catch (const IncompleteRelease&) {
            return;
        } catch (const std::exception& error) {
            std::cerr << "node release refresh failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "node release refresh failed with unknown error\n";
        }
    }

    class RefreshGuard final {
      public:
        explicit RefreshGuard(std::atomic_flag& value) noexcept : value_(value) {}
        ~RefreshGuard() { value_.clear(std::memory_order_release); }

        RefreshGuard(const RefreshGuard&) = delete;
        RefreshGuard& operator=(const RefreshGuard&) = delete;

      private:
        std::atomic_flag& value_;
    };

    std::chrono::nanoseconds probeInterval_;
    std::chrono::nanoseconds retiredReleaseRetention_;
    std::filesystem::path binaryPath_;
    std::filesystem::path installerPath_;
    std::filesystem::path manifestPath_;
    std::atomic<Snapshot> release_;
    std::atomic<std::int64_t> nextProbeNs_{};
    std::atomic_flag refreshing_ = ATOMIC_FLAG_INIT;
    std::vector<RetiredRelease> retiredReleases_;
};

inline Catalog& configuredRelease() {
    static Catalog value;
    return value;
}

inline void configure(const std::filesystem::path& binaryPath,
                      const std::filesystem::path& installerPath,
                      const std::filesystem::path& manifestPath) {
    configuredRelease().configure(binaryPath, installerPath, manifestPath);
}

inline Catalog::Snapshot current() { return configuredRelease().current(); }

} // namespace service::node_release
