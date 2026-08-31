#pragma once

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "node/proto/artifact.h"
#include "node/runtime/binary_digest.h"
#include "node/runtime/release_response.h"
#include "node/runtime/upgrade_record.h"

namespace flexedge::node {

struct SelfUpdaterConfig final {
    std::string serverOrigin;
    std::filesystem::path binaryPath;
    std::chrono::seconds checkInterval{15};
    std::string currentVersion;
    std::filesystem::path upgradeRecordPath;
    std::function<bool(std::string_view, std::string_view, std::string_view)> nodeLog;
    std::function<void()> requestRestart;
};

class SelfUpdater final {
  public:
    explicit SelfUpdater(SelfUpdaterConfig config)
        : config_(std::move(config)), currentDigest_(binarySha256(config_.binaryPath)) {}

    SelfUpdater(const SelfUpdater&) = delete;
    SelfUpdater& operator=(const SelfUpdater&) = delete;

    ~SelfUpdater() { stop(); }

    void start() {
        if (worker_.joinable()) {
            return;
        }
        validateConfig();
        // std::jthread invokes its callback with a stop_token by value.
        // NOLINTNEXTLINE(performance-unnecessary-value-param)
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    void stop() {
        if (!worker_.joinable()) {
            return;
        }
        worker_.request_stop();
        releaseAvailable_.notify_all();
        worker_.join();
    }

    [[nodiscard]] bool notifyRelease(std::string_view digest) {
        if (!isSha256Digest(digest) || digest == currentDigest_) {
            return false;
        }
        {
            std::lock_guard lock(releaseMutex_);
            pendingRelease_ = true;
        }
        releaseAvailable_.notify_one();
        return true;
    }

  private:
    bool log(std::string_view level, std::string_view message) const noexcept {
        try {
            return config_.nodeLog && config_.nodeLog(level, "upgrade", message);
        } catch (...) {
            return false;
        }
    }

    static std::string shellQuote(std::string_view value) {
        std::string result{"'"};
        for (const char ch : value) {
            if (ch == '\'') {
                result += "'\\''";
            } else {
                result.push_back(ch);
            }
        }
        result.push_back('\'');
        return result;
    }

    static void runCommand(const std::string& command) {
        const auto status = std::system(command.c_str());
        if (status != 0) {
            throw std::runtime_error("curl download failed");
        }
    }

    [[nodiscard]] std::string url(std::string_view path) const {
        auto origin = config_.serverOrigin;
        while (!origin.empty() && origin.back() == '/') {
            origin.pop_back();
        }
        return origin + std::string(path);
    }

    void downloadCandidate(const std::filesystem::path& candidate,
                           const std::filesystem::path& headers,
                           std::string_view currentDigest) const {
        const auto condition = "If-None-Match: " + nodeReleaseEntityTag(currentDigest);
        runCommand("curl -fsS --connect-timeout 5 --max-time 30 --dump-header " +
                   shellQuote(headers.string()) + " --output " + shellQuote(candidate.string()) +
                   " --header " + shellQuote(condition) + " " + shellQuote(url("/api/agent/node")));
    }

    [[nodiscard]] std::filesystem::path candidatePath() const {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto candidate = config_.binaryPath.parent_path();
        candidate /= "." + config_.binaryPath.filename().string() + ".upgrade." +
#ifdef _WIN32
                     "windows"
#else
                     std::to_string(::getpid())
#endif
                     + "." + std::to_string(now);
        return candidate;
    }

    void installCandidate(const std::filesystem::path& candidate) const {
#ifdef _WIN32
        (void)candidate;
        throw std::runtime_error("node self-upgrade is not supported on Windows");
#else
        if (::chmod(candidate.c_str(),
                    S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0) {
            throw std::runtime_error("failed to set node binary permissions");
        }
        std::error_code error;
        std::filesystem::rename(candidate, config_.binaryPath, error);
        if (error) {
            throw std::runtime_error("failed to replace node binary");
        }
#endif
    }

    bool upgradeOnce() const {
        const auto currentDigest = binarySha256(config_.binaryPath);
        const auto candidate = candidatePath();
        auto headers = candidate;
        headers += ".headers";
        try {
            downloadCandidate(candidate, headers, currentDigest);
            const auto response = readNodeReleaseResponse(headers);
            if (!response) {
                throw std::runtime_error("server returned invalid node release headers");
            }
            if (response->statusCode == 304) {
                if (response->sha256 != currentDigest) {
                    throw std::runtime_error("server returned an inconsistent 304 node release");
                }
                std::error_code ignored;
                std::filesystem::remove(candidate, ignored);
                std::filesystem::remove(headers, ignored);
                return false;
            }
            const auto candidateDigest = binarySha256(candidate);
            if (candidateDigest != response->sha256) {
                throw std::runtime_error("downloaded node binary digest mismatch");
            }
            const UpgradeRecord upgradeRecord{
                .previousVersion = config_.currentVersion,
                .currentVersion = response->version,
                .targetSha256 = response->sha256,
            };
            try {
                writePendingUpgradeRecord(config_.upgradeRecordPath, upgradeRecord);
            } catch (const std::exception& error) {
                std::cerr << "flexedge node could not persist upgrade record: " << error.what()
                          << '\n';
            }
            installCandidate(candidate);
            std::error_code ignored;
            std::filesystem::remove(headers, ignored);

            std::cerr << "flexedge node " << upgradeRecord.message() << "; restarting service\n";
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(candidate, ignored);
            std::filesystem::remove(headers, ignored);
            throw;
        }
        if (config_.requestRestart) {
            config_.requestRestart();
        }
        return true;
    }

    bool waitForCheck(const std::stop_token& token, std::chrono::seconds duration) {
        std::unique_lock lock(releaseMutex_);
        releaseAvailable_.wait_for(lock, token, duration, [this] { return pendingRelease_; });
        pendingRelease_ = false;
        return !token.stop_requested();
    }

    void run(const std::stop_token& token) {
        while (!token.stop_requested()) {
            try {
                if (upgradeOnce()) {
                    return;
                }
            } catch (const std::exception& error) {
                std::cerr << "flexedge node upgrade check failed: " << error.what() << '\n';
                (void)log("error", error.what());
            } catch (...) {
                std::cerr << "flexedge node upgrade check failed with unknown error\n";
                (void)log("error", "upgrade check failed with unknown error");
            }
            if (!waitForCheck(token, config_.checkInterval)) {
                return;
            }
        }
    }

    void validateConfig() const {
        if (!(config_.serverOrigin.starts_with("https://") ||
              config_.serverOrigin.starts_with("http://"))) {
            throw std::runtime_error("server origin is invalid");
        }
        if (config_.serverOrigin.find_first_of(" \t\r\n") != std::string::npos) {
            throw std::runtime_error("server origin must not contain whitespace");
        }
        if (config_.binaryPath.empty()) {
            throw std::runtime_error("node binary path is required");
        }
        if (config_.upgradeRecordPath.empty()) {
            throw std::runtime_error("node upgrade record path is required");
        }
        if (!validNodeReleaseVersion(config_.currentVersion)) {
            throw std::runtime_error("current node version is invalid");
        }
    }

    SelfUpdaterConfig config_;
    const std::string currentDigest_;
    std::mutex releaseMutex_;
    std::condition_variable_any releaseAvailable_;
    bool pendingRelease_{};
    std::jthread worker_;
};

} // namespace flexedge::node
