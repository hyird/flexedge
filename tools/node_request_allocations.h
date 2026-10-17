#pragma once

#include <cstdint>

namespace flexedge::node::benchmark {
// Counts ordinary C++ allocation calls on the calling thread; excludes aligned and C allocations.
void start_allocation_count() noexcept;
std::uint64_t stop_allocation_count() noexcept;
}  // namespace flexedge::node::benchmark
