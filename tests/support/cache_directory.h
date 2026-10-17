#pragma once

#include <chrono>
#include <filesystem>
#include <random>
#include <string>

namespace flexedge::testing {

struct temporary_cache_directory final {
    const std::filesystem::path path_ = std::filesystem::current_path() /
                                        ("node-cache-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(std::random_device{}()));
    ~temporary_cache_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
};

}  // namespace flexedge::testing
