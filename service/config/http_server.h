#pragma once

#include <ruvia/web/dotenv.h>
#include <ruvia/web/server_config.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace service::config {

struct http_server_settings final {
    ruvia::listen_config listen_;
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

inline http_server_settings get_http_server_settings(const ruvia::env& env,
    const std::filesystem::path& configuration_dir = std::filesystem::current_path()) {
    const auto host = env.get("HOST").value_or("127.0.0.1");
    if (host.empty()) {
        throw std::runtime_error("HOST must not be empty");
    }
    const auto port = env.get<std::uint16_t>("PORT").value_or(1102);
    if (port == 0) {
        throw std::runtime_error("PORT must be a positive TCP port");
    }

    const auto certificate_file = env.get("HTTPS_CERT_FILE").value_or("");
    const auto key_file = env.get("HTTPS_KEY_FILE").value_or("");
    if (certificate_file.empty() != key_file.empty()) {
        throw std::runtime_error("HTTPS_CERT_FILE and HTTPS_KEY_FILE must both be configured");
    }
    const auto redirect = env.get("HTTP_REDIRECT_TO_HTTPS").value_or("false");
    if (redirect != "true" && redirect != "false") {
        throw std::runtime_error("HTTP_REDIRECT_TO_HTTPS must be true or false");
    }

    ruvia::listen_config listener{.address_ = std::string(host), .http_ = port};
    const auto https_port = env.get<std::uint16_t>("HTTPS_PORT");
    if (certificate_file.empty()) {
        if (https_port.has_value() || redirect == "true") {
            throw std::runtime_error("HTTPS_PORT and HTTP_REDIRECT_TO_HTTPS=true require HTTPS_CERT_FILE and HTTPS_KEY_FILE");
        }
    } else {
        listener.https_ = https_port.value_or(443);
        if (*listener.https_ == 0 || *listener.https_ == port) {
            throw std::runtime_error("HTTPS_PORT must be a positive TCP port different from PORT");
        }
        listener.tls_ = {
            .certificate_chain_file_ = (configuration_dir / std::filesystem::path(std::u8string(certificate_file.begin(), certificate_file.end()))).lexically_normal(),
            .private_key_file_ = (configuration_dir / std::filesystem::path(std::u8string(key_file.begin(), key_file.end()))).lexically_normal(),
        };
        listener.auto_https_redirect_ = redirect == "true";
    }

    return {
        .listen_ = std::move(listener),
        .worker_count_ = http_server_detail::bounded_size(env, "WORKER_THREADS", 2, 1, 256),
        .blocking_threads_ = http_server_detail::bounded_size(env, "BLOCKING_THREADS", 2, 1, 256),
        .blocking_queue_capacity_ =
            http_server_detail::bounded_size(env, "BLOCKING_QUEUE_CAPACITY", 128, 1, 1024 * 1024),
        .compression_min_bytes_ = http_server_detail::bounded_size(env, "COMPRESSION_MIN_BYTES", 1024,
            0, 64 * 1024 * 1024),
    };
}

}  // namespace service::config
