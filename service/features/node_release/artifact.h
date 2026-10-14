#pragma once

#include <algorithm>
#include <array>
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

class artifact final {
public:
    explicit artifact(const std::filesystem::path& source_path) {
#ifdef _WIN32
        path_ = source_path;
        std::error_code error;
        source_size_ = std::filesystem::file_size(source_path, error);
        if (error) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        source_modified_ = std::filesystem::last_write_time(source_path, error);
        if (error) {
            throw std::runtime_error("node release artifact is unavailable");
        }
#else
        descriptor_ = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (descriptor_ < 0) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        struct stat metadata{};
        if (::fstat(descriptor_, &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
            ::close(descriptor_);
            descriptor_ = -1;
            throw std::runtime_error("node release artifact is not a regular file");
        }
        source_size_ = static_cast<std::uintmax_t>(metadata.st_size);
        source_stamp_ = stamp(metadata);
        path_ = "/proc/self/fd/" + std::to_string(descriptor_);
#endif
        try {
            digest_ = flexedge::crypto::file_sha256(path_);
            entity_tag_ = flexedge::node::node_release_entity_tag(digest_);
        } catch (...) {
#ifndef _WIN32
            ::close(descriptor_);
            descriptor_ = -1;
#endif
            throw;
        }
    }

    ~artifact() {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
#endif
    }

    artifact(const artifact&) = delete;
    artifact& operator=(const artifact&) = delete;
    artifact(artifact&&) = delete;
    artifact& operator=(artifact&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] const std::string& digest() const noexcept {
        return digest_;
    }
    [[nodiscard]] const std::string& entity_tag() const noexcept {
        return entity_tag_;
    }
    [[nodiscard]] std::uintmax_t size() const noexcept {
        return source_size_;
    }

    [[nodiscard]] std::string contents(std::size_t maximum_bytes) const {
        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            throw std::runtime_error("node release artifact is unavailable");
        }
        std::string result;
        std::array<char, 256> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(input.gcount());
            if (count > maximum_bytes - result.size()) {
                throw std::runtime_error("node release artifact exceeds size limit");
            }
            result.append(buffer.data(), count);
        }
        if (!input.eof()) {
            throw std::runtime_error("could not read node release artifact");
        }
        return result;
    }

    [[nodiscard]] std::string contents_range(std::uintmax_t offset, std::size_t maximum_bytes) const {
        if (maximum_bytes == 0 || offset > source_size_) {
            throw std::out_of_range("node release range is invalid");
        }
        const auto remaining = source_size_ - offset;
        const auto count = static_cast<std::size_t>(
            (std::min)(remaining, static_cast<std::uintmax_t>(maximum_bytes)));
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

    [[nodiscard]] bool matches_source(const std::filesystem::path& source_path) const noexcept {
#ifdef _WIN32
        std::error_code error;
        return std::filesystem::file_size(source_path, error) == source_size_ && !error &&
               std::filesystem::last_write_time(source_path, error) == source_modified_ && !error;
#else
        struct stat metadata{};
        return ::lstat(source_path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode) &&
               stamp(metadata) == source_stamp_;
#endif
    }

private:
#ifndef _WIN32
    struct source_stamp final {
        std::uint64_t device_{};
        std::uint64_t inode_{};
        std::uint64_t size_{};
        std::int64_t modified_seconds_{};
        std::int64_t modified_nanoseconds_{};

        bool operator==(const source_stamp&) const = default;
    };

    [[nodiscard]] static source_stamp stamp(const struct stat& metadata) noexcept {
#if defined(__APPLE__)
        return {.device_ = static_cast<std::uint64_t>(metadata.st_dev),
            .inode_ = static_cast<std::uint64_t>(metadata.st_ino),
            .size_ = static_cast<std::uint64_t>(metadata.st_size),
            .modified_seconds_ = metadata.st_mtimespec.tv_sec,
            .modified_nanoseconds_ = metadata.st_mtimespec.tv_nsec};
#else
        return {.device_ = static_cast<std::uint64_t>(metadata.st_dev),
            .inode_ = static_cast<std::uint64_t>(metadata.st_ino),
            .size_ = static_cast<std::uint64_t>(metadata.st_size),
            .modified_seconds_ = metadata.st_mtim.tv_sec,
            .modified_nanoseconds_ = metadata.st_mtim.tv_nsec};
#endif
    }

    int descriptor_{-1};
    source_stamp source_stamp_;
#else
    std::filesystem::file_time_type source_modified_{};
#endif
    std::uintmax_t source_size_{};
    std::filesystem::path path_;
    std::string digest_;
    std::string entity_tag_;
};

}  // namespace service::node_release
