#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace flexedge::node {

inline std::optional<std::string> read_secure_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        std::error_code error;
        if (std::filesystem::exists(path, error) || error) {
            throw std::runtime_error("could not read secure node state");
        }
        return std::nullopt;
    }
    std::string bytes;
    std::array<char, 16 * 1024> buffer{};
    while (stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
           stream.gcount() > 0) {
        bytes.append(buffer.data(), static_cast<std::size_t>(stream.gcount()));
    }
    if (stream.bad() || !stream.eof()) {
        throw std::runtime_error("could not read complete secure node state");
    }
    return bytes;
}

inline void write_secure_file_atomic(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
#ifndef _WIN32
    if (::chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
        throw std::runtime_error("could not restrict secure node state directory permissions");
    }
#endif
    auto temporary = path;
    static std::atomic<unsigned long long> sequence{0};
#ifdef _WIN32
    const auto process_id = GetCurrentProcessId();
#else
    const auto process_id = ::getpid();
#endif
    temporary += ".tmp." + std::to_string(process_id) + "." +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    struct temporary_cleanup {
        const std::filesystem::path& path_;
        bool owned_ = false;
        ~temporary_cleanup() {
            if (owned_) {
                std::error_code ignored;
                std::filesystem::remove(path_, ignored);
            }
        }
    } cleanup{temporary};
    {
#ifdef _WIN32
        const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("could not create secure node state temporary");
        }
        cleanup.owned_ = true;
        struct HandleGuard {
            HANDLE handle;
            ~HandleGuard() {
                if (handle != INVALID_HANDLE_VALUE) {
                    CloseHandle(handle);
                }
            }
        } guard{file};
        auto remaining = bytes;
        while (!remaining.empty()) {
            const auto chunk = static_cast<DWORD>(
                (std::min<std::size_t>)(remaining.size(), std::size_t{1} << 30));
            DWORD written{};
            if (!WriteFile(file, remaining.data(), chunk, &written, nullptr) || written == 0) {
                throw std::runtime_error("could not persist secure node state");
            }
            remaining.remove_prefix(written);
        }
        if (!FlushFileBuffers(file)) {
            throw std::runtime_error("could not persist secure node state");
        }
        if (!CloseHandle(std::exchange(guard.handle, INVALID_HANDLE_VALUE))) {
            throw std::runtime_error("could not close secure node state temporary");
        }
#else
        const auto descriptor = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
            S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            throw std::runtime_error("could not create secure node state temporary");
        }
        cleanup.owned_ = true;
        struct descriptor_guard {
            int descriptor_;
            ~descriptor_guard() {
                if (descriptor_ >= 0) {
                    ::close(descriptor_);
                }
            }
        } guard{descriptor};
        auto remaining = bytes;
        while (!remaining.empty()) {
            const auto written = ::write(descriptor, remaining.data(), remaining.size());
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::runtime_error("could not persist secure node state");
            }
            remaining.remove_prefix(static_cast<std::size_t>(written));
        }
        if (::fsync(descriptor) != 0) {
            throw std::runtime_error("could not persist secure node state");
        }
        if (::close(std::exchange(guard.descriptor_, -1)) != 0) {
            throw std::runtime_error("could not close secure node state temporary");
        }
#endif
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("could not atomically replace secure node state");
    }
#else
    if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        throw std::runtime_error("could not restrict secure node state permissions");
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        throw std::runtime_error("could not atomically replace secure node state");
    }
#endif
    cleanup.owned_ = false;
}

}  // namespace flexedge::node
