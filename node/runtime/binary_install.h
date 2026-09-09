#pragma once

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace flexedge::node {

[[nodiscard]] inline std::filesystem::path nodeUpgradeCandidatePath(const std::filesystem::path& binaryPath) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto candidate = binaryPath.parent_path();
    candidate /= "." + binaryPath.filename().string() + ".upgrade." +
#ifdef _WIN32
                 "windows"
#else
                 std::to_string(::getpid())
#endif
                 + "." + std::to_string(now);
    return candidate;
}

inline void removeNodeUpgradeCandidate(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

inline void installNodeUpgradeCandidate(const std::filesystem::path& candidate,
                                        const std::filesystem::path& binaryPath) {
#ifdef _WIN32
    (void)candidate;
    (void)binaryPath;
    throw std::runtime_error("node self-upgrade is not supported on Windows");
#else
    if (::chmod(candidate.c_str(),
                S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
        throw std::runtime_error("failed to set node binary permissions");
    }
    std::error_code error;
    std::filesystem::rename(candidate, binaryPath, error);
    if (error) {
        throw std::runtime_error("failed to replace node binary");
    }
#endif
}

} // namespace flexedge::node
