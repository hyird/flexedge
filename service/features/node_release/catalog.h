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

class Catalog final {
  public:
    using Snapshot = std::shared_ptr<const Release>;

    explicit Catalog(std::chrono::nanoseconds probeInterval = std::chrono::seconds(1),
                     std::chrono::nanoseconds retiredReleaseRetention = kRetiredReleaseRetention)
        : probeInterval_(probeInterval), retiredReleaseRetention_(retiredReleaseRetention) {}

    void configure(const std::filesystem::path& binaryPath,
                   const std::filesystem::path& installerPath,
                   const std::filesystem::path& manifestPath) {
        const std::scoped_lock lock(configurationMutex_);
        if (release_.load(std::memory_order_acquire)) {
            throw std::logic_error("node release is already configured");
        }
        auto release = std::make_shared<const Release>(binaryPath, installerPath, manifestPath);
        binaryPath_ = binaryPath;
        installerPath_ = installerPath;
        manifestPath_ = manifestPath;
        // The release publication also publishes its immutable source paths.
        release_.store(std::move(release), std::memory_order_release);
    }

    [[nodiscard]] Snapshot current() {
        auto release = release_.load(std::memory_order_acquire);
        if (!release) {
            throw std::logic_error("node release is not configured");
        }
        refreshIfDue();
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

    void refreshIfDue() noexcept {
        if (!claimRefresh() || refreshing_.test_and_set(std::memory_order_acq_rel)) {
            return;
        }
        const RefreshGuard guard(refreshing_);
        // Read under refresh ownership: a caller's earlier snapshot may already be retired.
        const auto release = release_.load(std::memory_order_acquire);
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
    std::mutex configurationMutex_;
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
