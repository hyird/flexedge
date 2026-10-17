#pragma once

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace flexedge::node {

class owned_listener_fd final {
public:
    owned_listener_fd() noexcept = default;
    explicit owned_listener_fd(int descriptor) noexcept
        : descriptor_(descriptor) {}
    owned_listener_fd(const owned_listener_fd&) = delete;
    owned_listener_fd& operator=(const owned_listener_fd&) = delete;
    owned_listener_fd(owned_listener_fd&& other) noexcept
        : descriptor_(other.release()) {}
    owned_listener_fd& operator=(owned_listener_fd&& other) noexcept {
        if (this != &other) {
            close();
            descriptor_ = other.release();
        }
        return *this;
    }
    ~owned_listener_fd() {
        close();
    }

    [[nodiscard]] int native_handle() const noexcept {
        return descriptor_;
    }
    [[nodiscard]] int release() noexcept {
        return std::exchange(descriptor_, -1);
    }

private:
    void close() noexcept {
#ifndef _WIN32
        if (descriptor_ >= 0) {
            // Retrying close after EINTR can close an unrelated, reused Linux FD.
            ::close(std::exchange(descriptor_, -1));
        }
#endif
    }
    int descriptor_ = -1;
};

class inherited_listeners final {
public:
    inherited_listeners() = default;
    inherited_listeners(const inherited_listeners&) = delete;
    inherited_listeners& operator=(const inherited_listeners&) = delete;
    inherited_listeners(inherited_listeners&&) noexcept = default;
    inherited_listeners& operator=(inherited_listeners&&) noexcept = default;

    [[nodiscard]] std::optional<owned_listener_fd> take(const asio::ip::tcp::endpoint& endpoint) {
        const auto found = std::find_if(listeners_.begin(), listeners_.end(),
            [&](const auto& listener) { return listener.first == endpoint; });
        if (found == listeners_.end()) {
            return std::nullopt;
        }
        owned_listener_fd descriptor{std::move(found->second)};
        listeners_.erase(found);
        return descriptor;
    }

    void close_unused() noexcept {
        listeners_.clear();
    }

    // Owner-loop shutdown only: process assembly now owns these queued listeners.
    void release_to_process() noexcept {
        for (auto& listener : listeners_) {
            (void)listener.second.release();
        }
        listeners_.clear();
    }

private:
    friend std::vector<inherited_listeners> consume_listener_handoff(std::size_t);
    std::vector<std::pair<asio::ip::tcp::endpoint, owned_listener_fd>> listeners_;
};

namespace listener_handoff_detail {

#ifndef _WIN32
// Call only during single-threaded startup, or after every old loop has joined.
// Never guess an FD upper bound from RLIMIT_NOFILE: it can be lowered beneath
// already-open descriptors. If neither directory is available, fail before exec.
template <typename function_type>
inline void for_each_descriptor(function_type&& function) {
    DIR* directory = ::opendir("/proc/self/fd");
    if (!directory) {
        directory = ::opendir("/dev/fd");
    }
    if (!directory) {
        throw std::system_error(errno, std::generic_category(),
            "could not enumerate node descriptors for listener handoff");
    }
    struct directory_guard {
        DIR* directory_;
        ~directory_guard() {
            ::closedir(directory_);
        }
    } guard{directory};
    const int enumeration_fd = ::dirfd(directory);
    for (;;) {
        errno = 0;
        const auto* entry = ::readdir(directory);
        if (!entry) {
            if (errno != 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not read node descriptors for listener handoff");
            }
            break;
        }
        const std::string_view name{entry->d_name};
        int descriptor = -1;
        const auto parsed = std::from_chars(name.data(), name.data() + name.size(), descriptor);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() &&
            descriptor >= 3 && descriptor != enumeration_fd) {
            function(descriptor);
        }
    }
}

inline std::optional<asio::ip::tcp::endpoint> listening_endpoint(int descriptor) {
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &value, &length) != 0 ||
        value != SOCK_STREAM) {
        return std::nullopt;
    }
    length = sizeof(value);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_ACCEPTCONN, &value, &length) != 0 || value != 1) {
        return std::nullopt;
    }
#ifdef SO_PROTOCOL
    length = sizeof(value);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_PROTOCOL, &value, &length) != 0 ||
        value != IPPROTO_TCP) {
        return std::nullopt;
    }
#endif
    asio::ip::tcp::endpoint endpoint;
    length = static_cast<socklen_t>(endpoint.capacity());
    if (::getsockname(descriptor, endpoint.data(), &length) != 0 ||
        (endpoint.data()->sa_family != AF_INET && endpoint.data()->sa_family != AF_INET6)) {
        return std::nullopt;
    }
    endpoint.resize(length);
    return endpoint;
}

inline void set_close_on_exec(int descriptor, bool enabled) {
    int flags;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not read node descriptor flags for listener handoff");
    }
    const int replacement = enabled ? flags | FD_CLOEXEC : flags & ~FD_CLOEXEC;
    if (replacement == flags) {
        return;
    }
    int result;
    do {
        result = ::fcntl(descriptor, F_SETFD, replacement);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not set node descriptor flags for listener handoff");
    }
}
#endif

inline std::size_t parse_number(std::string_view text) {
    std::size_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
        throw std::runtime_error("invalid node listener handoff number");
    }
    return value;
}

}  // namespace listener_handoff_detail

// Internal, one-shot format: v1;<old worker count>;<fd>,<fd>,...
// Invalid handoffs throw after closing inherited TCP listeners. The caller may
// report the error and start with empty bundles, binding fresh listening sockets.
inline std::vector<inherited_listeners> consume_listener_handoff(std::size_t worker_count) {
    const char* environment = std::getenv("FLEXEDGE_NODE_LISTEN_FDS");
    if (!environment) {
        return std::vector<inherited_listeners>(worker_count);
    }
#ifndef _WIN32
    struct environment_cleanup {
        ~environment_cleanup() {
            ::unsetenv("FLEXEDGE_NODE_LISTEN_FDS");
        }
    } cleanup;
    std::vector<std::pair<asio::ip::tcp::endpoint, owned_listener_fd>> inherited;
    try {
        const std::string marker{environment};
        if (::unsetenv("FLEXEDGE_NODE_LISTEN_FDS") != 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not consume node listener handoff environment");
        }
        listener_handoff_detail::for_each_descriptor([&](int descriptor) {
            if (auto endpoint = listener_handoff_detail::listening_endpoint(descriptor)) {
                owned_listener_fd owner{descriptor};
                inherited.emplace_back(*endpoint, std::move(owner));
            }
        });
        const auto separator = marker.find(';', 3);
        if (!marker.starts_with("v1;") || separator == std::string::npos || worker_count == 0 ||
            listener_handoff_detail::parse_number(
                std::string_view(marker).substr(3, separator - 3)) != worker_count) {
            throw std::runtime_error("node listener handoff worker count or version mismatch");
        }
        std::vector<int> listed;
        std::string_view remaining{marker.data() + separator + 1, marker.size() - separator - 1};
        while (!remaining.empty()) {
            const auto comma = remaining.find(',');
            const auto number = listener_handoff_detail::parse_number(remaining.substr(0, comma));
            if (number < 3 || number > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::runtime_error("invalid node listener handoff descriptor");
            }
            listed.push_back(static_cast<int>(number));
            if (comma == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(comma + 1);
            if (remaining.empty()) {
                throw std::runtime_error("invalid trailing node listener handoff delimiter");
            }
        }
        std::sort(listed.begin(), listed.end());
        if (std::adjacent_find(listed.begin(), listed.end()) != listed.end() ||
            listed.size() != inherited.size()) {
            throw std::runtime_error("node listener handoff descriptor list mismatch");
        }
        std::map<asio::ip::tcp::endpoint, std::vector<owned_listener_fd>> groups;
        std::vector<std::pair<dev_t, ino_t>> identities;
        identities.reserve(inherited.size());
        for (auto& [endpoint, descriptor] : inherited) {
            if (!std::binary_search(listed.begin(), listed.end(), descriptor.native_handle())) {
                throw std::runtime_error("node listener handoff contains an unlisted TCP listener");
            }
            struct stat status{};
            if (::fstat(descriptor.native_handle(), &status) != 0) {
                throw std::system_error(errno, std::generic_category(),
                    "could not inspect node listener handoff socket identity");
            }
            identities.emplace_back(status.st_dev, status.st_ino);
            // Inherited sockets must not escape an unrelated subsequent exec.
            listener_handoff_detail::set_close_on_exec(descriptor.native_handle(), true);
            groups[endpoint].push_back(std::move(descriptor));
        }
        std::sort(identities.begin(), identities.end());
        if (std::adjacent_find(identities.begin(), identities.end()) != identities.end()) {
            throw std::runtime_error("node listener handoff aliases one socket through multiple FDs");
        }
        for (const auto& [endpoint, descriptors] : groups) {
            (void)endpoint;
            if (descriptors.size() != worker_count) {
                throw std::runtime_error("node listener handoff endpoint has an incomplete worker group");
            }
        }
        std::vector<inherited_listeners> result(worker_count);
        for (auto& [endpoint, descriptors] : groups) {
            for (std::size_t worker = 0; worker < worker_count; ++worker) {
                result[worker].listeners_.emplace_back(endpoint, std::move(descriptors[worker]));
            }
        }
        return result;
    } catch (...) {
        inherited.clear();
        // Also cover allocation/parse failures before every inherited FD could
        // enter a container. No TCP listener may remain without an accept loop.
        try {
            listener_handoff_detail::for_each_descriptor([](int descriptor) {
                if (listener_handoff_detail::listening_endpoint(descriptor)) {
                    ::close(descriptor);
                }
            });
        } catch (...) {
            // The original error remains actionable; no exec occurs here.
        }
        throw;
    }
#else
    (void)worker_count;
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
        "node listener handoff requires POSIX descriptors");
#endif
}

// This function borrows, never closes, the old process's surviving descriptors.
// In particular an exec failure leaves every listener usable for rollback exec.
[[noreturn]] inline void reexec_with_listeners(const std::filesystem::path& binary_path,
    char* const argv[], std::size_t worker_count) {
#ifndef _WIN32
    if (worker_count == 0) {
        throw std::system_error(std::make_error_code(std::errc::invalid_argument),
            "node listener handoff requires at least one worker");
    }
    std::vector<int> descriptors;
    std::vector<int> listeners;
    listener_handoff_detail::for_each_descriptor([&](int descriptor) {
        descriptors.push_back(descriptor);
        if (listener_handoff_detail::listening_endpoint(descriptor)) {
            listeners.push_back(descriptor);
        }
    });
    std::string marker = "v1;" + std::to_string(worker_count) + ";";
    for (const int descriptor : listeners) {
        if (marker.back() != ';') {
            marker += ',';
        }
        marker += std::to_string(descriptor);
    }
    if (::setenv("FLEXEDGE_NODE_LISTEN_FDS", marker.c_str(), 1) != 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not publish node listener handoff");
    }
    // fcntl enumeration works without close_range or a particular Linux kernel.
    for (const int descriptor : descriptors) {
        listener_handoff_detail::set_close_on_exec(descriptor, true);
    }
    for (const int descriptor : listeners) {
        listener_handoff_detail::set_close_on_exec(descriptor, false);
    }
    ::execv(binary_path.c_str(), argv);
    const int error = errno;
    throw std::system_error(error, std::generic_category(),
        "could not re-execute node with inherited listeners");
#else
    (void)binary_path;
    (void)argv;
    (void)worker_count;
    throw std::system_error(std::make_error_code(std::errc::operation_not_supported),
        "node listener handoff requires POSIX descriptors");
#endif
}

}  // namespace flexedge::node
