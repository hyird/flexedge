#include "tools/node_request_allocations.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace {
thread_local bool count_allocations{};
thread_local std::uint64_t allocation_count{};
}  // namespace

namespace flexedge::node::benchmark {
void start_allocation_count() noexcept {
    allocation_count = 0;
    count_allocations = true;
}

std::uint64_t stop_allocation_count() noexcept {
    count_allocations = false;
    return allocation_count;
}
}  // namespace flexedge::node::benchmark

// A separate translation unit keeps allocator instrumentation outside the optimized request path.
void* operator new(std::size_t size) {
    if (void* result = std::malloc(size == 0 ? 1 : size)) {
        if (count_allocations) {
            ++allocation_count;
        }
        return result;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* value) noexcept {
    std::free(value);
}
void operator delete[](void* value) noexcept {
    std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
    std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept {
    std::free(value);
}
