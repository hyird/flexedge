#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <ruvia/web/Dotenv.h>

namespace service::config {

struct HttpServerSettings final {
    std::string host;
    std::uint16_t port;
    std::size_t workerCount;
    std::size_t blockingThreads;
    std::size_t blockingQueueCapacity;
    std::size_t compressionMinBytes;
};

namespace http_server_detail {

inline std::size_t boundedSize(const ruvia::Env& env, std::string_view name,
                               std::size_t defaultValue, std::size_t minimum, std::size_t maximum) {
    const auto value = env.get<std::size_t>(name).value_or(defaultValue);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(name) + " is outside the supported range");
    }
    return value;
}

} // namespace http_server_detail

inline HttpServerSettings httpServerSettings(const ruvia::Env& env) {
    const auto host = env.get("HOST").value_or("127.0.0.1");
    if (host.empty()) {
        throw std::runtime_error("HOST must not be empty");
    }
    const auto port = env.get<std::uint16_t>("PORT").value_or(1102);
    if (port == 0) {
        throw std::runtime_error("PORT must be a positive TCP port");
    }

    return {
        .host = std::string(host),
        .port = port,
        .workerCount = http_server_detail::boundedSize(env, "WORKER_THREADS", 2, 1, 256),
        .blockingThreads = http_server_detail::boundedSize(env, "BLOCKING_THREADS", 2, 1, 256),
        .blockingQueueCapacity =
            http_server_detail::boundedSize(env, "BLOCKING_QUEUE_CAPACITY", 128, 1, 1024 * 1024),
        .compressionMinBytes = http_server_detail::boundedSize(env, "COMPRESSION_MIN_BYTES", 1024,
                                                               0, 64 * 1024 * 1024),
    };
}

} // namespace service::config
