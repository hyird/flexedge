#include <ruvia/core/channel.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/signal_set.hpp>

#include "common/file_digest.h"
#include "node/control/control_channel.h"
#include "node/control/transport_config.h"
#include "node/data/data_plane.h"
#include "node/proto/control_protocol.h"
#include "node/runtime/listener_handoff.h"
#include "node/runtime/log_producer.h"
#include "node/runtime/loop_shutdown.h"
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

[[noreturn]] void reexec_node(const std::filesystem::path& binary_path, char* const argv[], std::size_t workers) {
    try {
        flexedge::node::reexec_with_listeners(binary_path, argv, workers);
    } catch (const std::exception& error) {
        std::cerr << "flexedge node candidate exec failed: " << error.what() << "; restoring previous binary\n";
        flexedge::node::restore_node_binary(binary_path);
        std::filesystem::remove("./state/pending-upgrade");
        flexedge::node::reexec_with_listeners(binary_path, argv, workers);
    }
}

ruvia::task<void> run_node_channel(flexedge::node::control_channel& channel,
    flexedge::node::log_producer& logs) {
    logs.node("info", "runtime", std::string("flexedge node ") + std::string(flexedge::node::node_version) + " starting");
    co_await channel.run();
}

}  // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path binary_path;
    std::size_t worker_count{};
    bool rollback_startup{};
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--check-upgrade") {
            std::cout << "FLEXEDGE_NODE_UPGRADE_READY " << flexedge::node::node_version << '\n';
            return 0;
        }
        if (argc != 3) {
            throw std::runtime_error("usage: node <server-domain> <credentials-file>");
        }
        std::cerr << "flexedge node " << flexedge::node::node_version << " starting\n";
        auto web_socket = flexedge::node::control_transport_config(argv[1]);
        ruvia::event_loop_pool control_loops({.loop_count_ = 1});
        ruvia::event_loop_pool worker_loops;
        worker_count = worker_loops.loop_count();
        const auto loop = control_loops.loop(0);
        flexedge::node::runtime_state runtime;
        auto [data_log_sender, data_log_receiver] =
            ruvia::make_channel<flexedge::node::owned_log_batch>(loop.handle(),
                {.capacity_ = flexedge::node::data_log_channel_capacity});
        auto [control_log_sender, control_log_receiver] =
            ruvia::make_channel<flexedge::node::owned_log_batch>(loop.handle(),
                {.capacity_ = flexedge::node::control_log_channel_capacity});
        flexedge::node::log_producer control_logs(loop, std::move(control_log_sender));
        const std::filesystem::path state_directory{"./state"};
        binary_path = executable_path(argv[0]);
        const auto current_digest = flexedge::crypto::file_sha256(binary_path);
        rollback_startup = flexedge::node::read_pending_upgrade_record(state_directory / "pending-upgrade", current_digest).has_value();
        auto credentials = flexedge::node::node_credentials::load(argv[2]);
        auto inherited = flexedge::node::consume_listener_handoff(worker_count);
        flexedge::node::data_plane data_plane(worker_loops, loop, runtime, data_log_sender,
            state_directory / "content_cache", std::move(inherited));
        ruvia::stop_source stop_source;
        std::atomic<bool> shutting_down{};
        std::atomic<bool> restart_requested{};
        const auto request_stop = [&](std::string_view reason) {
            if (!shutting_down.exchange(true)) {
                std::cerr << "flexedge node " << reason << '\n';
            }
            data_plane.request_stop_accepting(restart_requested.load());
            stop_source.request_stop();
        };
        asio::signal_set signals(loop.io_context(), SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code& error, int) {
            if (error) {
                return;
            }
            request_stop("shutdown requested");
        });
        // An internal upgrade requests stop without delivering a process signal.
        // Retire the outstanding Asio wait on its owner loop before joining it.
        auto signals_stop = loop.on_stop([&signals]() -> ruvia::task<void> {
            std::error_code ignored;
            signals.cancel(ignored);
            co_return;
        });
        flexedge::node::self_updater self_updater({
            .binary_path_ = binary_path,
            .current_version_ = std::string(flexedge::node::node_version),
            .current_sha256_ = current_digest,
            .upgrade_record_path_ = state_directory / "pending-upgrade",
            .node_log_ =
                [&control_logs](std::string_view level, std::string_view category,
                    std::string_view message) {
                    return control_logs.node(level, category, message);
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
            data_plane, control_logs, self_updater,
            std::move(data_log_receiver), std::move(control_log_receiver));
        flexedge::node::loop_shutdown loop_shutdown(worker_loops, control_loops);
        worker_loops.start();
        control_loops.start();
        data_plane.start();
        auto task = loop.start(run_node_channel(channel, control_logs));
        std::exception_ptr failure;
        try {
            task.get();
        } catch (...) {
            failure = std::current_exception();
        }
        rollback_startup = self_updater.startup_pending();
        if (shutting_down.load() || rollback_startup) {
            if (rollback_startup) {
                data_plane.request_stop_accepting(true);
            }
            // Stop admission first; keep existing responses alive up to the bounded deadline.
            (void)loop.start(data_plane.wait_drained(std::chrono::seconds(10))).get();
        }
        loop_shutdown.stop_and_join();
        if (failure && !shutting_down.load()) {
            std::rethrow_exception(failure);
        }
        if (restart_requested.load()) {
            reexec_node(binary_path, argv, worker_count);
        }
        return 0;
    } catch (const std::exception& error) {
        if (rollback_startup && !binary_path.empty() &&
            std::filesystem::exists(flexedge::node::previous_node_binary_path(binary_path))) {
            try {
                std::cerr << "flexedge node candidate startup failed: " << error.what() << "; restoring previous binary\n";
                flexedge::node::restore_node_binary(binary_path);
                std::filesystem::remove("./state/pending-upgrade");
                flexedge::node::reexec_with_listeners(binary_path, argv, worker_count);
            } catch (const std::exception& rollback) {
                std::cerr << "flexedge node rollback failed: " << rollback.what() << '\n';
            }
        }
        std::cerr << "flexedge node failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "flexedge node failed with unknown exception\n";
        return 1;
    }
}
