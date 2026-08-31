#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
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

#include "node/control/control_channel.h"
#include "node/control/log_channel.h"
#include "node/data/data_plane.h"
#include "node/proto/control_protocol.h"
#include "node/runtime/binary_digest.h"
#include "node/runtime/log_buffer.h"
#include "node/runtime/node_credentials.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/upgrade_record.h"

namespace {

ruvia::WebSocketClientConfig webSocketConfig(std::string_view address) {
    auto scheme = ruvia::WebSocketScheme::kWss;
    if (address.starts_with("wss://")) {
        address.remove_prefix(6);
    } else if (address.starts_with("ws://")) {
        scheme = ruvia::WebSocketScheme::kWs;
        address.remove_prefix(5);
    }
    if (address.empty() || address.contains('@') || address.contains('?') ||
        address.contains('#')) {
        throw std::runtime_error("server address is invalid");
    }
    auto authority = address;
    std::string target{"/api/agent/connect"};
    if (const auto slash = address.find('/'); slash != std::string_view::npos) {
        authority = address.substr(0, slash);
        target = std::string(address.substr(slash));
    }
    std::string host;
    std::optional<std::uint16_t> port;
    std::string_view portText;
    if (authority.starts_with('[')) {
        const auto closing = authority.find(']');
        if (closing == std::string_view::npos) {
            throw std::runtime_error("server address is invalid");
        }
        host = std::string(authority.substr(1, closing - 1));
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') {
                throw std::runtime_error("server address is invalid");
            }
            portText = authority.substr(closing + 2);
        }
    } else if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = std::string(authority.substr(0, colon));
        portText = authority.substr(colon + 1);
    } else {
        host = std::string(authority);
    }
    if (host.empty()) {
        throw std::runtime_error("server domain is required");
    }
    if (!portText.empty()) {
        unsigned int parsed{};
        const auto result =
            std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != portText.data() + portText.size() ||
            parsed == 0 || parsed > 65535) {
            throw std::runtime_error("server port is invalid");
        }
        port = static_cast<std::uint16_t>(parsed);
    }
    return {
        .scheme = scheme,
        .host = std::move(host),
        .port = port,
        .target = std::move(target),
        .subprotocols = {std::string(flexedge::node::kControlSubprotocol)},
        .maxMessageBytes = 4 * 1024 * 1024,
        .connectTimeout = std::chrono::seconds(10),
        .readTimeout = std::chrono::seconds(5),
        .writeTimeout = std::chrono::seconds(30),
        .closeHandshakeTimeout = std::chrono::seconds(5),
        .userAgent = flexedge::node::nodeUserAgent(),
    };
}

std::string httpOrigin(const ruvia::WebSocketClientConfig& config) {
    std::string origin = config.scheme == ruvia::WebSocketScheme::kWss ? "https://" : "http://";
    if (config.host.contains(':')) {
        origin += "[" + config.host + "]";
    } else {
        origin += config.host;
    }
    if (config.port) {
        origin += ":" + std::to_string(*config.port);
    }
    return origin;
}

std::string updateOrigin(const ruvia::WebSocketClientConfig& config) {
    if (const char* value = std::getenv("FLEXEDGE_SERVER_ORIGIN"); value && *value) {
        return value;
    }
    return httpOrigin(config);
}

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
        auto webSocket = webSocketConfig(argv[1]);
        auto serverOrigin = updateOrigin(webSocket);
        ruvia::EventLoopPool controlLoops({.loopCount = 1});
        ruvia::EventLoopPool workerLoops;
        const auto loop = controlLoops.loop(0);
        flexedge::node::RuntimeState runtime;
        flexedge::node::NodeLogBuffer logBuffer(workerLoops.loopCount());
        const auto updaterLogs = logBuffer.backgroundProducer();
        const std::filesystem::path stateDirectory{"./state"};
        const auto binaryPath = executablePath(argv[0]);
        auto credentials = flexedge::node::NodeCredentials::load(argv[2]);
        if (const auto upgrade = flexedge::node::takePendingUpgradeRecord(
                stateDirectory / "pending-upgrade", flexedge::node::binarySha256(binaryPath))) {
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
            .serverOrigin = std::move(serverOrigin),
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
        flexedge::node::ControlChannel channel(
            loop,
            {
                .webSocket = webSocket,
                .stopToken = stopSource.token(),
                .nodeReleaseAvailable =
                    [&selfUpdater](std::string_view digest) {
                        (void)selfUpdater.notifyRelease(digest);
                    },
            },
            flexedge::node::StateStore{stateDirectory, credentials.secret()}, credentials, runtime,
            dataPlane, logBuffer);
        flexedge::node::LogChannel logChannel(loop,
                                              {
                                                  .webSocket = std::move(webSocket),
                                                  .stopToken = stopSource.token(),
                                              },
                                              credentials, logBuffer);
        workerLoops.start();
        controlLoops.start();
        dataPlane.start();
        auto task = loop.start(channel.run());
        auto logTask = loop.start(logChannel.run());
        selfUpdater.start();
        auto stopLoops = [&] {
            workerLoops.stop();
            controlLoops.stop();
            workerLoops.join();
            controlLoops.join();
        };
        try {
            task.get();
        } catch (...) {
            stopLoops();
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
        stopLoops();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "flexedge node failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "flexedge node failed with unknown exception\n";
        return 1;
    }
}
