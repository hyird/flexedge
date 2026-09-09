#pragma once

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "common/file_digest.h"
#include "node/proto/release_metadata.h"

namespace service::node_release {

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
        sourceSize_ = static_cast<std::uintmax_t>(metadata.st_size);
        sourceStamp_ = stamp(metadata);
        path_ = "/proc/self/fd/" + std::to_string(descriptor_);
#endif
        try {
            digest_ = flexedge::crypto::fileSha256(path_);
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
    [[nodiscard]] std::uintmax_t size() const noexcept { return sourceSize_; }

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

    [[nodiscard]] std::string contentsRange(std::uintmax_t offset, std::size_t maximumBytes) const {
        if (maximumBytes == 0 || offset > sourceSize_) {
            throw std::out_of_range("node release range is invalid");
        }
        const auto remaining = sourceSize_ - offset;
        const auto count = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uintmax_t>(maximumBytes)));
        if (count == 0) {
            return {};
        }
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!input) {
            throw std::runtime_error("could not seek node release artifact");
        }
        std::string result(count, '\0');
        input.read(result.data(), static_cast<std::streamsize>(result.size()));
        if (input.gcount() != static_cast<std::streamsize>(result.size())) {
            throw std::runtime_error("could not read node release artifact range");
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
    std::filesystem::file_time_type sourceModified_{};
#endif
    std::uintmax_t sourceSize_{};
    std::filesystem::path path_;
    std::string digest_;
    std::string entityTag_;
};

} // namespace service::node_release
