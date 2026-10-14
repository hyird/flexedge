#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#include "service/features/logging/connection.h"
#include "service/features/logging/logger.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

class capturing_buffer final : public std::streambuf {
public:
    [[nodiscard]] bool wait_for(std::string_view value, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout,
            [&] { return content_.find(value) != std::string::npos; });
    }

    [[nodiscard]] std::string content() const {
        std::lock_guard lock(mutex_);
        return content_;
    }

protected:
    std::streamsize xsputn(const char* bytes, std::streamsize size) override {
        std::lock_guard lock(mutex_);
        content_.append(bytes, static_cast<std::size_t>(size));
        return size;
    }

    int overflow(int value) override {
        if (value == traits_type::eof()) {
            return traits_type::not_eof(value);
        }
        std::lock_guard lock(mutex_);
        content_.push_back(static_cast<char>(value));
        return value;
    }

    int sync() override {
        changed_.notify_all();
        return 0;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string content_;
};

}  // namespace

int main() {
    capturing_buffer info_buffer;
    capturing_buffer error_buffer;
    std::ostream info_output(&info_buffer);
    std::ostream error_output(&error_buffer);
    service::logging::async_logger logger(info_output, error_output, 20000, 16 * 1024 * 1024);
    REQUIRE(!logger.enqueue(service::logging::level::info, "inactive"));
    logger.start();
    REQUIRE(logger.enqueue(service::logging::level::info, "realtime-marker"));
    REQUIRE(info_buffer.wait_for("realtime-marker", std::chrono::milliseconds(500)));

    constexpr std::size_t producer_count = 16;
    constexpr std::size_t messages_per_producer = 1000;
    std::atomic<std::size_t> accepted{};
    std::vector<std::thread> producers;
    producers.reserve(producer_count);
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t sequence = 0; sequence < messages_per_producer; ++sequence) {
                if (logger.enqueue(service::logging::level::info,
                        "producer=" + std::to_string(producer) +
                            " sequence=" + std::to_string(sequence))) {
                    ++accepted;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }
    REQUIRE(logger.enqueue(service::logging::level::error, "error-marker"));
    logger.stop();
    REQUIRE(accepted == producer_count * messages_per_producer);
    REQUIRE(!logger.enqueue(service::logging::level::info, "stopped"));

    const auto info = info_buffer.content();
    const auto errors = error_buffer.content();
    REQUIRE(std::ranges::count(info, '\n') == producer_count * messages_per_producer + 1);
    REQUIRE(info.find("realtime-marker") != std::string::npos);
    REQUIRE(errors.find("ERROR error-marker") != std::string::npos);
    REQUIRE(std::ranges::count(errors, '\n') == 1);

    capturing_buffer bounded_info_buffer;
    capturing_buffer bounded_error_buffer;
    std::ostream bounded_info(&bounded_info_buffer);
    std::ostream bounded_error(&bounded_error_buffer);
    service::logging::async_logger bounded(bounded_info, bounded_error, 1, 64);
    bounded.start();
    REQUIRE(!bounded.enqueue(service::logging::level::info, std::string(128, 'x')));
    bounded.stop();
    REQUIRE(bounded_error_buffer.content().find("dropped 1 messages") != std::string::npos);

    const auto broken_pipe = std::make_exception_ptr(
        std::system_error(std::make_error_code(std::errc::broken_pipe), "client write failed"));
    try {
        std::rethrow_exception(broken_pipe);
    } catch (const std::exception& error) {
        REQUIRE(service::common::peer_disconnected(error));
        REQUIRE(service::common::sse_client_disconnected(error));
    }
    const auto application_failure =
        std::make_exception_ptr(std::runtime_error("database connection failed"));
    try {
        std::rethrow_exception(application_failure);
    } catch (const std::exception& error) {
        REQUIRE(!service::common::peer_disconnected(error));
        REQUIRE(!service::common::sse_client_disconnected(error));
    }
    REQUIRE(service::common::sse_client_disconnected(
        std::system_error(std::make_error_code(std::errc::connection_aborted), "write failed")));
    const std::runtime_error cancelled("redis operation cancelled");
    REQUIRE(service::common::sse_client_disconnected(cancelled));
    REQUIRE(!service::common::peer_disconnected(cancelled));
    return 0;
}

#undef REQUIRE
