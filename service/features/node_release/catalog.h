#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include "service/features/node_release/release.h"

namespace service::node_release {

class catalog final {
public:
    using snapshot = std::shared_ptr<const release>;

    explicit catalog(std::chrono::nanoseconds probe_interval = std::chrono::seconds(1),
        std::chrono::nanoseconds retired_release_retention_value = retired_release_retention)
        : probe_interval_(probe_interval),
          retired_release_retention_(retired_release_retention_value) {}

    void configure(const std::filesystem::path& binary_path,
        const std::filesystem::path& installer_path,
        const std::filesystem::path& manifest_path) {
        const std::scoped_lock lock(configuration_mutex_);
        if (release_.load(std::memory_order_acquire)) {
            throw std::logic_error("node release is already configured");
        }
        auto release_value = std::make_shared<const release>(binary_path, installer_path, manifest_path);
        binary_path_ = binary_path;
        installer_path_ = installer_path;
        manifest_path_ = manifest_path;
        // The release publication also publishes its immutable source paths.
        release_.store(std::move(release_value), std::memory_order_release);
    }

    [[nodiscard]] snapshot current() {
        auto release = release_.load(std::memory_order_acquire);
        if (!release) {
            throw std::logic_error("node release is not configured");
        }
        refresh_if_due();
        return release_.load(std::memory_order_acquire);
    }

private:
    static constexpr auto retired_release_retention = std::chrono::minutes(2);

    struct retired_release final {
        snapshot snapshot_;
        std::int64_t expires_at_ns_{};
    };

    [[nodiscard]] static std::int64_t monotonic_now_ns() noexcept {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] bool claim_refresh() noexcept {
        const auto now = monotonic_now_ns();
        auto due = next_probe_ns_.load(std::memory_order_relaxed);
        while (now >= due) {
            const auto next = now + probe_interval_.count();
            if (next_probe_ns_.compare_exchange_weak(due, next, std::memory_order_relaxed)) {
                return true;
            }
        }
        return false;
    }

    void refresh_if_due() noexcept {
        if (!claim_refresh() || refreshing_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        const refresh_guard guard(refreshing_);
        // Read under refresh ownership: a caller's earlier snapshot may already be retired.
        const auto release_value = release_.load(std::memory_order_acquire);
        const auto now = monotonic_now_ns();
        std::erase_if(retired_releases_,
            [now](const retired_release& retired) { return retired.expires_at_ns_ <= now; });
        if (release_value->matches_sources(binary_path_, installer_path_, manifest_path_)) {
            return;
        }
        try {
            auto replacement =
                std::make_shared<const release>(binary_path_, installer_path_, manifest_path_);
            // c.file() reopens this descriptor path after its handler returns. Retaining prior
            // releases through the response write timeout keeps an in-flight download stable.
            retired_releases_.push_back(
                {.snapshot_ = release_value, .expires_at_ns_ = now + retired_release_retention_.count()});
            release_.store(std::move(replacement), std::memory_order_release);
        } catch (const incomplete_release&) {
            return;
        } catch (const std::exception& error) {
            std::cerr << "node release refresh failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "node release refresh failed with unknown error\n";
        }
    }

    class refresh_guard final {
    public:
        explicit refresh_guard(std::atomic_flag& value) noexcept
            : value_(value) {}
        ~refresh_guard() {
            value_.clear(std::memory_order_release);
        }

        refresh_guard(const refresh_guard&) = delete;
        refresh_guard& operator=(const refresh_guard&) = delete;

    private:
        std::atomic_flag& value_;
    };

    std::chrono::nanoseconds probe_interval_;
    std::chrono::nanoseconds retired_release_retention_;
    std::filesystem::path binary_path_;
    std::filesystem::path installer_path_;
    std::filesystem::path manifest_path_;
    std::mutex configuration_mutex_;
    std::atomic<snapshot> release_;
    std::atomic<std::int64_t> next_probe_ns_{};
    std::atomic_flag refreshing_ = ATOMIC_FLAG_INIT;
    std::vector<retired_release> retired_releases_;
};

inline catalog& configured_release() {
    static catalog value;
    return value;
}

inline void configure(const std::filesystem::path& binary_path,
    const std::filesystem::path& installer_path,
    const std::filesystem::path& manifest_path) {
    configured_release().configure(binary_path, installer_path, manifest_path);
}

inline catalog::snapshot current() {
    return configured_release().current();
}

}  // namespace service::node_release
