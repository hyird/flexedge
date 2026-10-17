#pragma once

#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace flexedge::node {

inline ruvia::task<void> check_node_upgrade_candidate(const ruvia::worker_handle& worker,
    const std::filesystem::path& candidate, std::string_view version, ruvia::stop_token stop = {}) {
#if defined(__linux__)
    if (::chmod(candidate.c_str(), 0755) != 0) {
        throw std::system_error(errno, std::generic_category(), "could not make node candidate executable");
    }
    std::array<int, 2> pipe;
    if (::pipe2(pipe.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        throw std::system_error(errno, std::generic_category(), "could not open node candidate check pipe");
    }
    struct pipe_guard final {
        std::array<int, 2>& pipe_;
        ~pipe_guard() {
            for (const int descriptor : pipe_) {
                if (descriptor >= 0) {
                    ::close(descriptor);
                }
            }
        }
    } pipes{pipe};
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "could not prepare node candidate check");
    }
    struct actions_guard final {
        posix_spawn_file_actions_t& actions_;
        ~actions_guard() {
            ::posix_spawn_file_actions_destroy(&actions_);
        }
    } action_owner{actions};
    error = ::posix_spawn_file_actions_adddup2(&actions, pipe[1], STDOUT_FILENO);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "could not connect node candidate check output");
    }
    auto path = candidate.string();
    char check_argument[] = "--check-upgrade";
    std::array<char*, 3> arguments{path.data(), check_argument, nullptr};
    pid_t child{};
    error = ::posix_spawn(&child, path.c_str(), &actions, nullptr, arguments.data(), environ);
    if (error != 0) {
        throw std::system_error(error, std::generic_category(), "node candidate could not start");
    }
    struct child_guard final {
        pid_t child_;
        ~child_guard() {
            if (child_ > 0) {
                ::kill(child_, SIGKILL);
                int status{};
                while (::waitpid(child_, &status, 0) < 0 && errno == EINTR) {
                }
            }
        }
    } process{child};
    ::close(pipe[1]);
    pipe[1] = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int status{};
    for (;;) {
        const auto completed = ::waitpid(child, &status, WNOHANG);
        if (completed == child) {
            process.child_ = -1;
            break;
        }
        if (completed < 0 && errno != EINTR) {
            throw std::system_error(errno, std::generic_category(), "could not reap node candidate check");
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("node candidate check cancelled or timed out");
        }
        // Only the local child lifecycle is sampled; the control WebSocket keeps
        // processing heartbeat/log mailboxes on this same owner loop.
        (void)co_await ruvia::sleep_for(worker, std::chrono::milliseconds(10), stop);
    }
    std::string output;
    std::array<char, 256> buffer;
    for (;;) {
        const auto bytes = ::read(pipe[0], buffer.data(), buffer.size());
        if (bytes > 0) {
            output.append(buffer.data(), static_cast<std::size_t>(bytes));
            if (output.size() > 512) {
                throw std::runtime_error("node candidate check output exceeded its limit");
            }
            continue;
        }
        if (bytes < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        output != "FLEXEDGE_NODE_UPGRADE_READY " + std::string(version) + "\n") {
        throw std::runtime_error("node candidate failed its startup capability check");
    }
#else
    (void)worker;
    (void)candidate;
    (void)version;
    (void)stop;
    throw std::runtime_error("node self-upgrade requires Linux");
#endif
    co_return;
}

}  // namespace flexedge::node
