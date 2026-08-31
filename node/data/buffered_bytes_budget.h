#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <system_error>
#include <utility>

namespace flexedge::node {

class BufferedBytesBudget;

class BufferedBytesLease final {
  public:
    BufferedBytesLease() = default;
    BufferedBytesLease(const BufferedBytesLease&) = delete;
    BufferedBytesLease& operator=(const BufferedBytesLease&) = delete;

    BufferedBytesLease(BufferedBytesLease&& other) noexcept { moveFrom(std::move(other)); }

    BufferedBytesLease& operator=(BufferedBytesLease&& other) noexcept {
        if (this != &other) {
            reset();
            moveFrom(std::move(other));
        }
        return *this;
    }

    ~BufferedBytesLease() { reset(); }

    [[nodiscard]] bool tryGrow(std::size_t bytes) noexcept;

    void shrink(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }

  private:
    friend class BufferedBytesBudget;

    explicit BufferedBytesLease(BufferedBytesBudget& budget) noexcept : budget_(&budget) {}

    void reset() noexcept;

    void moveFrom(BufferedBytesLease&& other) noexcept {
        budget_ = std::exchange(other.budget_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }

    BufferedBytesBudget* budget_{};
    std::size_t bytes_{};
};

class BufferedBytesBudget final {
  public:
    explicit BufferedBytesBudget(std::size_t maximumBytes) noexcept : maximumBytes_(maximumBytes) {}

    [[nodiscard]] BufferedBytesLease lease() noexcept { return BufferedBytesLease(*this); }

    [[nodiscard]] std::size_t usedBytes() const noexcept {
        return usedBytes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t rejectedReservations() const noexcept {
        return rejectedReservations_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t maximumBytes() const noexcept { return maximumBytes_; }

  private:
    friend class BufferedBytesLease;

    [[nodiscard]] bool reserve(std::size_t bytes) noexcept {
        if (bytes == 0) {
            return true;
        }
        auto used = usedBytes_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > maximumBytes_ || used > maximumBytes_ - bytes) {
                rejectedReservations_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (usedBytes_.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed)) {
                return true;
            }
        }
    }

    void release(std::size_t bytes) noexcept {
        if (bytes != 0) {
            usedBytes_.fetch_sub(bytes, std::memory_order_acq_rel);
        }
    }

    std::size_t maximumBytes_{};
    std::atomic<std::size_t> usedBytes_{};
    std::atomic<std::size_t> rejectedReservations_{};
};

inline bool BufferedBytesLease::tryGrow(std::size_t bytes) noexcept {
    if (budget_ == nullptr) {
        return true;
    }
    if (!budget_->reserve(bytes)) {
        return false;
    }
    bytes_ += bytes;
    return true;
}

inline void BufferedBytesLease::shrink(std::size_t bytes) noexcept {
    if (budget_ == nullptr || bytes == 0) {
        return;
    }
    const auto released = (std::min)(bytes, bytes_);
    bytes_ -= released;
    budget_->release(released);
}

inline void BufferedBytesLease::reset() noexcept {
    if (budget_ != nullptr) {
        budget_->release(std::exchange(bytes_, 0));
    }
    budget_ = nullptr;
}

inline bool bufferedBytesExhausted(const std::error_code& error) noexcept {
    return error == std::make_error_code(std::errc::not_enough_memory);
}

inline constexpr std::size_t kMaximumRequestBufferBytes{128 * 1024 * 1024};
inline constexpr std::size_t kMaximumResponseBufferBytes{256 * 1024 * 1024};

} // namespace flexedge::node
