#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace test_support {

inline std::optional<std::string> environment_variable(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    const auto error = _dupenv_s(&value, &size, name);
    const std::unique_ptr<char, decltype(&std::free)> owned(value, &std::free);
    if (error != 0) {
        throw std::runtime_error("Cannot read test environment variable");
    }
    if (owned) {
        return std::string(owned.get());
    }
#else
    if (const auto* value = std::getenv(name)) {
        return std::string(value);
    }
#endif
    return std::nullopt;
}

}  // namespace test_support
