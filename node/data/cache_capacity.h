#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

#ifdef _WIN32
// Windows socket headers must precede windows.h.
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

namespace flexedge::node {
struct cache_resource_capacity final {
    std::uint64_t total_{};
    std::uint64_t available_{};
};

inline std::optional<cache_resource_capacity> cache_memory_capacity() {
#ifdef _WIN32
    MEMORYSTATUSEX value{.dwLength = sizeof(value)};
    if (GlobalMemoryStatusEx(&value)) {
        return cache_resource_capacity{value.ullTotalPhys, value.ullAvailPhys};
    }
#elif defined(__APPLE__)
    std::uint64_t total{};
    std::size_t total_size = sizeof(total);
    vm_statistics64_data_t memory{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_size_t page_size{};
    const auto host = mach_host_self();
    const bool read = sysctlbyname("hw.memsize", &total, &total_size, nullptr, 0) == 0 &&
                      host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count) == KERN_SUCCESS &&
                      host_page_size(host, &page_size) == KERN_SUCCESS;
    mach_port_deallocate(mach_task_self(), host);
    if (read) {
        return cache_resource_capacity{total, (static_cast<std::uint64_t>(memory.free_count) + memory.inactive_count) * page_size};
    }
#else
    std::ifstream input("/proc/meminfo");
    std::string key, unit;
    std::uint64_t value{}, total{}, available{};
    bool has_available{};
    while (input >> key >> value >> unit) {
        if (key == "MemTotal:") {
            total = value * 1024;
        }
        if (key == "MemAvailable:") {
            available = value * 1024;
            has_available = true;
        }
        if (total && has_available) {
            return cache_resource_capacity{total, available};
        }
    }
#endif
    return std::nullopt;
}

// Available capacity already excludes existing cache allocations. Add this
// policy's usage back before limiting it, so a full cache can replace its LRU
// entries without shrinking its own allowance on every write.
inline std::uint64_t cache_capacity_limit(std::optional<cache_resource_capacity> resource,
    std::uint64_t used, std::uint64_t fixed, std::uint64_t percent, std::uint64_t reserved) {
    if (!resource) {
        return percent || reserved ? 0 : fixed;
    }
    const auto configured = percent ? (resource->total_ / 100 * percent + resource->total_ % 100 * percent / 100) : fixed;
    const auto available = (std::min)(resource->available_, resource->total_);
    const auto recoverable = (std::min)(resource->total_, available + (std::min)(used, resource->total_ - available));
    return (std::min)(configured, recoverable > reserved ? recoverable - reserved : 0);
}
}  // namespace flexedge::node
