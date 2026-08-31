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

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("requirement failed: " #condition);                           \
    } while (false)

namespace {

class CapturingBuffer final : public std::streambuf {
  public:
    [[nodiscard]] bool waitFor(std::string_view value, std::chrono::milliseconds timeout) {
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

} // namespace

int main() {
    CapturingBuffer infoBuffer;
    CapturingBuffer errorBuffer;
    std::ostream infoOutput(&infoBuffer);
    std::ostream errorOutput(&errorBuffer);
    service::logging::AsyncLogger logger(infoOutput, errorOutput, 20000, 16 * 1024 * 1024);
    REQUIRE(!logger.enqueue(service::logging::Level::info, "inactive"));
    logger.start();
    REQUIRE(logger.enqueue(service::logging::Level::info, "realtime-marker"));
    REQUIRE(infoBuffer.waitFor("realtime-marker", std::chrono::milliseconds(500)));

    constexpr std::size_t producerCount = 16;
    constexpr std::size_t messagesPerProducer = 1000;
    std::atomic<std::size_t> accepted{};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);
    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t sequence = 0; sequence < messagesPerProducer; ++sequence) {
                if (logger.enqueue(service::logging::Level::info,
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
    REQUIRE(logger.enqueue(service::logging::Level::error, "error-marker"));
    logger.stop();
    REQUIRE(accepted == producerCount * messagesPerProducer);
    REQUIRE(!logger.enqueue(service::logging::Level::info, "stopped"));

    const auto info = infoBuffer.content();
    const auto errors = errorBuffer.content();
    REQUIRE(std::ranges::count(info, '\n') == producerCount * messagesPerProducer + 1);
    REQUIRE(info.contains("realtime-marker"));
    REQUIRE(errors.contains("ERROR error-marker"));
    REQUIRE(std::ranges::count(errors, '\n') == 1);

    CapturingBuffer boundedInfoBuffer;
    CapturingBuffer boundedErrorBuffer;
    std::ostream boundedInfo(&boundedInfoBuffer);
    std::ostream boundedError(&boundedErrorBuffer);
    service::logging::AsyncLogger bounded(boundedInfo, boundedError, 1, 64);
    bounded.start();
    REQUIRE(!bounded.enqueue(service::logging::Level::info, std::string(128, 'x')));
    bounded.stop();
    REQUIRE(boundedErrorBuffer.content().contains("dropped 1 messages"));

    const auto brokenPipe = std::make_exception_ptr(
        std::system_error(std::make_error_code(std::errc::broken_pipe), "client write failed"));
    try {
        std::rethrow_exception(brokenPipe);
    } catch (const std::exception& error) {
        REQUIRE(service::logging::peerDisconnected(error));
    }
    const auto applicationFailure =
        std::make_exception_ptr(std::runtime_error("database connection failed"));
    try {
        std::rethrow_exception(applicationFailure);
    } catch (const std::exception& error) {
        REQUIRE(!service::logging::peerDisconnected(error));
    }
    return 0;
}

#undef REQUIRE
