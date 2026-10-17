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

[[nodiscard]] inline std::filesystem::path node_upgrade_candidate_path(const std::filesystem::path& binary_path) {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto candidate = binary_path.parent_path();
    candidate /= "." + binary_path.filename().string() + ".upgrade." +
#ifdef _WIN32
                 "windows"
#else
                 std::to_string(::getpid())
#endif
                 + "." + std::to_string(now);
    return candidate;
}

inline void remove_node_upgrade_candidate(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] inline std::filesystem::path previous_node_binary_path(const std::filesystem::path& binary_path) {
    return binary_path.parent_path() / ("." + binary_path.filename().string() + ".previous");
}

inline void preserve_node_binary(const std::filesystem::path& binary_path) {
    std::filesystem::create_hard_link(binary_path, previous_node_binary_path(binary_path));
}

inline void restore_node_binary(const std::filesystem::path& binary_path) {
    std::filesystem::rename(previous_node_binary_path(binary_path), binary_path);
}

inline void install_node_upgrade_candidate(const std::filesystem::path& candidate,
    const std::filesystem::path& binary_path) {
#ifdef _WIN32
    (void)candidate;
    (void)binary_path;
    throw std::runtime_error("node self-upgrade is not supported on Windows");
#else
    std::error_code error;
    std::filesystem::rename(candidate, binary_path, error);
    if (error) {
        throw std::runtime_error("failed to replace node binary");
    }
#endif
}

}  // namespace flexedge::node
