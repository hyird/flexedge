#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>

namespace flexedge::node {

class buffered_bytes_budget;

class buffered_bytes_lease final {
public:
    buffered_bytes_lease() = default;
    buffered_bytes_lease(const buffered_bytes_lease&) = delete;
    buffered_bytes_lease& operator=(const buffered_bytes_lease&) = delete;

    buffered_bytes_lease(buffered_bytes_lease&& other) noexcept {
        move_from(std::move(other));
    }

    buffered_bytes_lease& operator=(buffered_bytes_lease&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    ~buffered_bytes_lease() {
        reset();
    }

    [[nodiscard]] bool try_grow(std::size_t bytes) noexcept;

    void shrink(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    friend class buffered_bytes_budget;

    explicit buffered_bytes_lease(buffered_bytes_budget& budget) noexcept
        : budget_(&budget) {}

    void reset() noexcept;

    void move_from(buffered_bytes_lease&& other) noexcept {
        budget_ = std::exchange(other.budget_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }

    buffered_bytes_budget* budget_{};
    std::size_t bytes_{};
};

class buffered_bytes_budget final {
public:
    explicit buffered_bytes_budget(std::size_t maximum_bytes) noexcept
        : maximum_bytes_(maximum_bytes) {}

    [[nodiscard]] buffered_bytes_lease lease() noexcept {
        return buffered_bytes_lease(*this);
    }

    [[nodiscard]] std::size_t used_bytes() const noexcept {
        return used_bytes_;
    }

    [[nodiscard]] std::size_t rejected_reservations() const noexcept {
        return rejected_reservations_;
    }

    [[nodiscard]] std::size_t maximum_bytes() const noexcept {
        return maximum_bytes_;
    }

private:
    friend class buffered_bytes_lease;

    [[nodiscard]] bool reserve(std::size_t bytes) noexcept {
        if (bytes == 0) {
            return true;
        }
        if (bytes > maximum_bytes_ || used_bytes_ > maximum_bytes_ - bytes) {
            ++rejected_reservations_;
            return false;
        }
        used_bytes_ += bytes;
        return true;
    }

    void release(std::size_t bytes) noexcept {
        if (bytes != 0) {
            used_bytes_ -= bytes;
        }
    }

    std::size_t maximum_bytes_{};
    std::size_t used_bytes_{};
    std::size_t rejected_reservations_{};
};

inline bool buffered_bytes_lease::try_grow(std::size_t bytes) noexcept {
    if (budget_ == nullptr) {
        return true;
    }
    if (!budget_->reserve(bytes)) {
        return false;
    }
    bytes_ += bytes;
    return true;
}

inline void buffered_bytes_lease::shrink(std::size_t bytes) noexcept {
    if (budget_ == nullptr || bytes == 0) {
        return;
    }
    const auto released = (std::min)(bytes, bytes_);
    bytes_ -= released;
    budget_->release(released);
}

inline void buffered_bytes_lease::reset() noexcept {
    if (budget_ != nullptr) {
        budget_->release(std::exchange(bytes_, 0));
    }
    budget_ = nullptr;
}

inline constexpr std::size_t maximum_request_buffer_bytes{128 * 1024 * 1024};
inline constexpr std::size_t maximum_response_buffer_bytes{256 * 1024 * 1024};

}  // namespace flexedge::node
