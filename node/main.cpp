#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/signal_set.hpp>

#include "node/runtime/loop_shutdown.h"

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>

#include "common/file_digest.h"
#include "node/control/control_channel.h"
#include "node/control/transport_config.h"
#include "node/data/data_plane.h"
#include "node/proto/control_protocol.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/upgrade_record.h"

namespace {

std::filesystem::path executable_path(std::string_view fallback) {
#ifndef _WIN32
    std::error_code proc_error;
    const auto proc_path = std::filesystem::read_symlink("/proc/self/exe", proc_error);
    if (!proc_error && !proc_path.empty()) {
        return proc_path;
    }
#endif
    std::filesystem::path path{std::string(fallback)};
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    std::error_code canonical_error;
    const auto canonical = std::filesystem::weakly_canonical(path, canonical_error);
    return canonical_error ? path : canonical;
}

[[noreturn]] void reexec_node(const std::filesystem::path& binary_path, char* const argv[]) {
#if defined(__linux__)
    if (::syscall(SYS_close_range, 3U, std::numeric_limits<unsigned int>::max(), 0U) == 0) {
        ::execv(binary_path.c_str(), argv);
    }
#else
    (void)binary_path;
    (void)argv;
#endif
    std::cerr << "flexedge node could not re-execute upgraded binary\n";
    std::_Exit(EXIT_FAILURE);
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            throw std::runtime_error("usage: node <server-domain> <credentials-file>");
        }
        std::cerr << "flexedge node " << flexedge::node::node_version << " starting\n";
        auto web_socket = flexedge::node::control_transport_config(argv[1]);
        ruvia::event_loop_pool control_loops({.loop_count_ = 1});
        ruvia::event_loop_pool worker_loops;
        const auto loop = control_loops.loop(0);
        flexedge::node::runtime_state runtime;
        auto [log_queued_sender, log_queued_receiver] =
            ruvia::make_channel<bool>(loop.handle(), {.capacity_ = 1});
        flexedge::node::node_log_buffer log_buffer(worker_loops.loop_count(),
            [log_queued_sender] { (void)log_queued_sender.send(true); });
        const auto updater_logs = log_buffer.background_producer();
        const std::filesystem::path state_directory{"./state"};
        const auto binary_path = executable_path(argv[0]);
        auto credentials = flexedge::node::node_credentials::load(argv[2]);
        if (const auto upgrade = flexedge::node::take_pending_upgrade_record(
                state_directory / "pending-upgrade", flexedge::crypto::file_sha256(binary_path))) {
            log_buffer.node("info", "upgrade", upgrade->message());
        }
        std::string startup_log{"flexedge node "};
        startup_log.append(flexedge::node::node_version);
        startup_log.append(" starting");
        log_buffer.node("info", "runtime", startup_log);
        flexedge::node::data_plane data_plane(worker_loops, loop, runtime, log_buffer);
        ruvia::stop_source stop_source;
        std::atomic<bool> shutting_down{};
        std::atomic<bool> restart_requested{};
        const auto request_stop = [&](std::string_view reason) {
            if (!shutting_down.exchange(true)) {
                std::cerr << "flexedge node " << reason << '\n';
            }
            data_plane.request_stop_accepting();
            stop_source.request_stop();
        };
        asio::signal_set signals(loop.io_context(), SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code& error, int) {
            if (error) {
                return;
            }
            request_stop("shutdown requested");
        });
        flexedge::node::self_updater self_updater({
            .binary_path_ = binary_path,
            .current_version_ = std::string(flexedge::node::node_version),
            .upgrade_record_path_ = state_directory / "pending-upgrade",
            .node_log_ =
                [updater_logs](std::string_view level, std::string_view category,
                    std::string_view message) {
                    return updater_logs.node(level, category, message);
                },
            .request_restart_ =
                [&] {
                    restart_requested = true;
                    request_stop("upgrade restart requested");
                },
        });
        auto control_web_socket = web_socket;
        control_web_socket.read_timeout_ =
            std::chrono::seconds(flexedge::node::release_watch_maximum_seconds + 5);
        flexedge::node::control_channel channel(
            loop,
            {
                .web_socket_ = std::move(control_web_socket),
                .stop_token_ = stop_source.token(),
            },
            flexedge::node::state_store{state_directory, credentials.secret()}, credentials, runtime,
            data_plane, log_buffer, self_updater, std::move(log_queued_receiver));
        flexedge::node::loop_shutdown loop_shutdown(worker_loops, control_loops);
        worker_loops.start();
        control_loops.start();
        data_plane.start();
        auto task = loop.start(channel.run());
        try {
            task.get();
        } catch (...) {
            loop_shutdown.stop_and_join();
            if (!shutting_down.load()) {
                throw;
            }
            if (restart_requested.load()) {
                reexec_node(binary_path, argv);
            }
            return 0;
        }
        if (shutting_down.load()) {
            // Each worker reports when its last connection closes; stop after 10 s regardless.
            (void)loop.start(data_plane.wait_drained(std::chrono::seconds(10))).get();
        }
        loop_shutdown.stop_and_join();
        if (restart_requested.load()) {
            reexec_node(binary_path, argv);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "flexedge node failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "flexedge node failed with unknown exception\n";
        return 1;
    }
}
