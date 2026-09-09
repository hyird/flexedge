#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace flexedge::node {

inline std::optional<std::string> readSecureFile(const std::filesystem::path& path) {
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

inline void writeSecureFileAtomic(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
#ifndef _WIN32
    if (::chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
        throw std::runtime_error("could not restrict secure node state directory permissions");
    }
#endif
    auto temporary = path;
    static std::atomic<unsigned long long> sequence{0};
#ifdef _WIN32
    const auto processId = GetCurrentProcessId();
#else
    const auto processId = ::getpid();
#endif
    temporary += ".tmp." + std::to_string(processId) + "." +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    struct TemporaryCleanup {
        const std::filesystem::path& path;
        bool owned = false;
        ~TemporaryCleanup() {
            if (owned) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } cleanup{temporary};
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::out | std::ios::noreplace);
        if (!stream) throw std::runtime_error("could not create secure node state temporary");
        cleanup.owned = true;
        if (!stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            !stream.flush()) {
            throw std::runtime_error("could not persist secure node state");
        }
        stream.close();
        if (!stream) throw std::runtime_error("could not close secure node state temporary");
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("could not atomically replace secure node state");
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
    cleanup.owned = false;
}

} // namespace flexedge::node
