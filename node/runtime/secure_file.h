#pragma once

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
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

inline void writeSecureFileAtomic(const std::filesystem::path& path, std::string_view bytes) {
    std::filesystem::create_directories(path.parent_path());
#ifndef _WIN32
    if (::chmod(path.parent_path().c_str(), S_IRWXU) != 0) {
        throw std::runtime_error("could not restrict secure node state directory permissions");
    }
#endif
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream || !stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) ||
            !stream.flush()) {
            throw std::runtime_error("could not persist secure node state");
        }
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
}

} // namespace flexedge::node
