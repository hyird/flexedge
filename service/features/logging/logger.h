#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace service::logging {

enum class Level : std::uint8_t { info, error };

struct Event final {
    std::chrono::system_clock::time_point occurredAt;
    Level level;
    std::string message;
};

class AsyncLogger final {
  public:
    static constexpr std::size_t kDefaultMaxEvents{100000};
    static constexpr std::size_t kDefaultMaxBytes{64 * 1024 * 1024};

    AsyncLogger(std::ostream& infoOutput, std::ostream& errorOutput,
                std::size_t maxEvents = kDefaultMaxEvents, std::size_t maxBytes = kDefaultMaxBytes)
        : infoOutput_(infoOutput), errorOutput_(errorOutput), maxEvents_(maxEvents),
          maxBytes_(maxBytes) {
        if (maxEvents_ == 0 || maxBytes_ == 0) {
            throw std::invalid_argument("logger queue capacity must be positive");
        }
    }

    ~AsyncLogger() { stop(); }

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;
    AsyncLogger(AsyncLogger&&) = delete;
    AsyncLogger& operator=(AsyncLogger&&) = delete;

    void start() {
        std::lock_guard lock(mutex_);
        if (running_) {
            return;
        }
        stopping_ = false;
        running_ = true;
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            running_ = false;
            throw;
        }
    }

    void stop() noexcept {
        {
            std::lock_guard lock(mutex_);
            if (!running_) {
                return;
            }
            stopping_ = true;
        }
        ready_.notify_one();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard lock(mutex_);
        running_ = false;
        stopping_ = false;
    }

    [[nodiscard]] bool enqueue(Level level, std::string message) noexcept {
        try {
            Event event{
                .occurredAt = std::chrono::system_clock::now(),
                .level = level,
                .message = std::move(message),
            };
            const auto bytes = event.message.size() + sizeof(Event);
            bool notify = false;
            bool accepted = false;
            {
                std::lock_guard lock(mutex_);
                if (!running_ || stopping_) {
                    return false;
                }
                if (bytes > maxBytes_ || queue_.size() >= maxEvents_ ||
                    queuedBytes_ > maxBytes_ - bytes) {
                    ++droppedSinceReport_;
                    notify = true;
                } else {
                    notify = queue_.empty();
                    queuedBytes_ += bytes;
                    queue_.push_back(std::move(event));
                    accepted = true;
                }
            }
            if (notify) {
                ready_.notify_one();
            }
            return accepted;
        } catch (...) {
            return false;
        }
    }

  private:
    static std::string timestamp(std::chrono::system_clock::time_point value) {
        const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(value);
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(value - seconds).count();
        const auto time = std::chrono::system_clock::to_time_t(seconds);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &time);
#else
        localtime_r(&time, &local);
#endif
        std::ostringstream output;
        output << std::put_time(&local, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
               << std::setw(3) << milliseconds;
        return output.str();
    }

    static void write(std::ostream& output, std::string_view level, const Event& event) {
        output << '[' << timestamp(event.occurredAt) << "] " << level << ' ' << event.message
               << '\n';
    }

    void run() noexcept {
        std::deque<Event> batch;
        while (true) {
            std::uint64_t dropped{};
            bool stopAfterBatch = false;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopping_ || !queue_.empty() || droppedSinceReport_ != 0;
                });
                batch.swap(queue_);
                queuedBytes_ = 0;
                dropped = std::exchange(droppedSinceReport_, 0);
                stopAfterBatch = stopping_;
            }

            for (const auto& event : batch) {
                if (event.level == Level::error) {
                    write(errorOutput_, "ERROR", event);
                } else {
                    write(infoOutput_, "INFO", event);
                }
            }
            batch.clear();
            if (dropped != 0) {
                const Event warning{
                    .occurredAt = std::chrono::system_clock::now(),
                    .level = Level::error,
                    .message = "Server log queue dropped " + std::to_string(dropped) +
                               " messages due to bounded capacity",
                };
                write(errorOutput_, "ERROR", warning);
            }
            infoOutput_.flush();
            errorOutput_.flush();
            if (stopAfterBatch) {
                return;
            }
        }
    }

    std::ostream& infoOutput_;
    std::ostream& errorOutput_;
    const std::size_t maxEvents_;
    const std::size_t maxBytes_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Event> queue_;
    std::size_t queuedBytes_{};
    std::uint64_t droppedSinceReport_{};
    bool running_{};
    bool stopping_{};
    std::thread worker_;
};

inline AsyncLogger& logger() {
    static AsyncLogger value(std::cout, std::cerr);
    return value;
}

class Session final {
  public:
    Session() { logger().start(); }
    ~Session() { logger().stop(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;
};

inline void info(std::string message) noexcept {
    (void)logger().enqueue(Level::info, std::move(message));
}

inline void error(std::string message) noexcept {
    (void)logger().enqueue(Level::error, std::move(message));
}

} // namespace service::logging
