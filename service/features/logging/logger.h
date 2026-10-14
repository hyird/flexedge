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

enum class level : std::uint8_t { info,
    error };

struct event final {
    std::chrono::system_clock::time_point occurred_at_;
    level level_;
    std::string message_;
};

class async_logger final {
public:
    static constexpr std::size_t default_max_events{100000};
    static constexpr std::size_t default_max_bytes{64 * 1024 * 1024};

    async_logger(std::ostream& info_output, std::ostream& error_output,
        std::size_t max_events = default_max_events, std::size_t max_bytes = default_max_bytes)
        : info_output_(info_output),
          error_output_(error_output),
          max_events_(max_events),
          max_bytes_(max_bytes) {
        if (max_events_ == 0 || max_bytes_ == 0) {
            throw std::invalid_argument("logger queue capacity must be positive");
        }
    }

    ~async_logger() {
        stop();
    }

    async_logger(const async_logger&) = delete;
    async_logger& operator=(const async_logger&) = delete;
    async_logger(async_logger&&) = delete;
    async_logger& operator=(async_logger&&) = delete;

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

    [[nodiscard]] bool enqueue(level level, std::string message) noexcept {
        try {
            event event_value{
                .occurred_at_ = std::chrono::system_clock::now(),
                .level_ = level,
                .message_ = std::move(message),
            };
            const auto bytes = event_value.message_.size() + sizeof(event);
            bool notify = false;
            bool accepted = false;
            {
                std::lock_guard lock(mutex_);
                if (!running_ || stopping_) {
                    return false;
                }
                if (bytes > max_bytes_ || queue_.size() >= max_events_ ||
                    queued_bytes_ > max_bytes_ - bytes) {
                    ++dropped_since_report_;
                    notify = true;
                } else {
                    notify = queue_.empty();
                    queued_bytes_ += bytes;
                    queue_.push_back(std::move(event_value));
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

    static void write(std::ostream& output, std::string_view level, const event& event) {
        output << '[' << timestamp(event.occurred_at_) << "] " << level << ' ' << event.message_
               << '\n';
    }

    void run() noexcept {
        std::deque<event> batch;
        while (true) {
            std::uint64_t dropped{};
            bool stop_after_batch = false;
            {
                std::unique_lock lock(mutex_);
                ready_.wait(lock, [this] {
                    return stopping_ || !queue_.empty() || dropped_since_report_ != 0;
                });
                batch.swap(queue_);
                queued_bytes_ = 0;
                dropped = std::exchange(dropped_since_report_, 0);
                stop_after_batch = stopping_;
            }

            for (const auto& event : batch) {
                if (event.level_ == level::error) {
                    write(error_output_, "ERROR", event);
                } else {
                    write(info_output_, "INFO", event);
                }
            }
            batch.clear();
            if (dropped != 0) {
                const event warning{
                    .occurred_at_ = std::chrono::system_clock::now(),
                    .level_ = level::error,
                    .message_ = "Server log queue dropped " + std::to_string(dropped) +
                                " messages due to bounded capacity",
                };
                write(error_output_, "ERROR", warning);
            }
            info_output_.flush();
            error_output_.flush();
            if (stop_after_batch) {
                return;
            }
        }
    }

    std::ostream& info_output_;
    std::ostream& error_output_;
    const std::size_t max_events_;
    const std::size_t max_bytes_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<event> queue_;
    std::size_t queued_bytes_{};
    std::uint64_t dropped_since_report_{};
    bool running_{};
    bool stopping_{};
    std::thread worker_;
};

inline async_logger& logger() {
    static async_logger value(std::cout, std::cerr);
    return value;
}

class session final {
public:
    session() {
        logger().start();
    }
    ~session() {
        logger().stop();
    }

    session(const session&) = delete;
    session& operator=(const session&) = delete;
    session(session&&) = delete;
    session& operator=(session&&) = delete;
};

inline void info(std::string message) noexcept {
    (void)logger().enqueue(level::info, std::move(message));
}

inline void error(std::string message) noexcept {
    (void)logger().enqueue(level::error, std::move(message));
}

}  // namespace service::logging
