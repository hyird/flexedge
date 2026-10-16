#pragma once

#include <memory_resource>

namespace test_support {

// What task_service needs from a request context.
template <typename handle_type>
struct read_context final {
    handle_type handle_;
    std::pmr::memory_resource* pool_;

    handle_type& db() {
        return handle_;
    }
    [[nodiscard]] std::pmr::memory_resource* pool() const {
        return pool_;
    }
};

}  // namespace test_support
