#include "node/runtime/loop_shutdown.h"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include <asio/signal_set.hpp>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/StopToken.h>

#include "node/control/transport_config.h"
#include "node/control/control_channel.h"
#include "node/control/log_channel.h"
#include "node/data/data_plane.h"
#include "node/proto/control_protocol.h"
#include "common/file_digest.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/upgrade_record.h"

namespace {

std::filesystem::path executablePath(std::string_view fallback) {
#ifndef _WIN32
    std::error_code procError;
    const auto procPath = std::filesystem::read_symlink("/proc/self/exe", procError);
    if (!procError && !procPath.empty()) {
        return procPath;
    }
#endif
    std::filesystem::path path{std::string(fallback)};
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }
    std::error_code canonicalError;
    const auto canonical = std::filesystem::weakly_canonical(path, canonicalError);
    return canonicalError ? path : canonical;
}

[[noreturn]] void reexecNode(const std::filesystem::path& binaryPath, char* const argv[]) {
#if defined(__linux__)
    if (::syscall(SYS_close_range, 3U, std::numeric_limits<unsigned int>::max(), 0U) == 0) {
        ::execv(binaryPath.c_str(), argv);
    }
#else
    (void)binaryPath;
    (void)argv;
#endif
    std::cerr << "flexedge node could not re-execute upgraded binary\n";
    std::_Exit(EXIT_FAILURE);
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            throw std::runtime_error("usage: node <server-domain> <credentials-file>");
        }
        std::cerr << "flexedge node " << flexedge::node::kNodeVersion << " starting\n";
        auto webSocket = flexedge::node::controlTransportConfig(argv[1]);
        ruvia::EventLoopPool controlLoops({.loopCount = 1});
        ruvia::EventLoopPool workerLoops;
        const auto loop = controlLoops.loop(0);
        flexedge::node::RuntimeState runtime;
        auto [logQueuedSender, logQueuedReceiver] =
            ruvia::makeChannel<bool>(loop.handle(), {.capacity = 1});
        flexedge::node::NodeLogBuffer logBuffer(workerLoops.loopCount(),
            [logQueuedSender] { (void)logQueuedSender.send(true); });
        const auto updaterLogs = logBuffer.backgroundProducer();
        const std::filesystem::path stateDirectory{"./state"};
        const auto binaryPath = executablePath(argv[0]);
        auto credentials = flexedge::node::NodeCredentials::load(argv[2]);
        if (const auto upgrade = flexedge::node::takePendingUpgradeRecord(
                stateDirectory / "pending-upgrade", flexedge::crypto::fileSha256(binaryPath))) {
            logBuffer.node("info", "upgrade", upgrade->message());
        }
        std::string startupLog{"flexedge node "};
        startupLog.append(flexedge::node::kNodeVersion);
        startupLog.append(" starting");
        logBuffer.node("info", "runtime", startupLog);
        flexedge::node::DataPlane dataPlane(workerLoops, loop, runtime, logBuffer);
        ruvia::StopSource stopSource;
        std::atomic<bool> shuttingDown{};
        const auto requestStop = [&](std::string_view reason) {
            if (!shuttingDown.exchange(true)) {
                std::cerr << "flexedge node " << reason << '\n';
            }
            dataPlane.requestStopAccepting();
            stopSource.requestStop();
        };
        asio::signal_set signals(loop.ioContext(), SIGINT, SIGTERM);
        signals.async_wait([&](const std::error_code& error, int) {
            if (error) {
                return;
            }
            requestStop("shutdown requested");
        });
        flexedge::node::SelfUpdater selfUpdater({
            .binaryPath = binaryPath,
            .currentVersion = std::string(flexedge::node::kNodeVersion),
            .upgradeRecordPath = stateDirectory / "pending-upgrade",
            .nodeLog =
                [updaterLogs](std::string_view level, std::string_view category,
                              std::string_view message) {
                    return updaterLogs.node(level, category, message);
                },
            .requestRestart =
                [&, binaryPath] {
                    dataPlane.requestStopAccepting();
                    reexecNode(binaryPath, argv);
                },
        });
        auto controlWebSocket = webSocket;
        controlWebSocket.readTimeout =
            std::chrono::seconds(flexedge::node::kReleaseWatchMaximumSeconds + 5);
        flexedge::node::ControlChannel channel(
            loop,
            {
                .webSocket = std::move(controlWebSocket),
                .stopToken = stopSource.token(),
            },
            flexedge::node::StateStore{stateDirectory, credentials.secret()}, credentials, runtime,
            dataPlane, logBuffer, selfUpdater);
        flexedge::node::LogChannel logChannel(loop,
                                              {
                                                  .webSocket = std::move(webSocket),
                                                  .stopToken = stopSource.token(),
                                              },
                                              credentials, logBuffer, std::move(logQueuedReceiver));
        flexedge::node::LoopShutdown loopShutdown(workerLoops, controlLoops);
        workerLoops.start();
        controlLoops.start();
        dataPlane.start();
        auto task = loop.start(channel.run());
        auto logTask = loop.start(logChannel.run());
        try {
            task.get();
        } catch (...) {
            loopShutdown.stopAndJoin();
            if (!shuttingDown.load()) {
                throw;
            }
            return 0;
        }
        if (shuttingDown.load()) {
            const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!dataPlane.drained() && std::chrono::steady_clock::now() < drainDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            logTask.get();
        }
        loopShutdown.stopAndJoin();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "flexedge node failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "flexedge node failed with unknown exception\n";
        return 1;
    }
}
