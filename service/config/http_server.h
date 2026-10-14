#pragma once

#include <ruvia/web/dotenv.h>

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace service::config {

struct http_server_settings final {
    std::string host_;
    std::uint16_t port_;
    std::size_t worker_count_;
    std::size_t blocking_threads_;
    std::size_t blocking_queue_capacity_;
    std::size_t compression_min_bytes_;
};

namespace http_server_detail {

inline std::size_t bounded_size(const ruvia::env& env, std::string_view name,
    std::size_t default_value, std::size_t minimum, std::size_t maximum) {
    const auto value = env.get<std::size_t>(name).value_or(default_value);
    if (value < minimum || value > maximum) {
        throw std::runtime_error(std::string(name) + " is outside the supported range");
    }
    return value;
}

}  // namespace http_server_detail

inline http_server_settings get_http_server_settings(const ruvia::env& env) {
    const auto host = env.get("HOST").value_or("127.0.0.1");
    if (host.empty()) {
        throw std::runtime_error("HOST must not be empty");
    }
    const auto port = env.get<std::uint16_t>("PORT").value_or(1102);
    if (port == 0) {
        throw std::runtime_error("PORT must be a positive TCP port");
    }

    return {
        .host_ = std::string(host),
        .port_ = port,
        .worker_count_ = http_server_detail::bounded_size(env, "WORKER_THREADS", 2, 1, 256),
        .blocking_threads_ = http_server_detail::bounded_size(env, "BLOCKING_THREADS", 2, 1, 256),
        .blocking_queue_capacity_ =
            http_server_detail::bounded_size(env, "BLOCKING_QUEUE_CAPACITY", 128, 1, 1024 * 1024),
        .compression_min_bytes_ = http_server_detail::bounded_size(env, "COMPRESSION_MIN_BYTES", 1024,
            0, 64 * 1024 * 1024),
    };
}

}  // namespace service::config
