#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

namespace flexedge::node {

struct runtime_metrics_snapshot final {
    double cpu_usage_{};
    double memory_usage_{};
    std::int64_t traffic_out_bps_{};
    double load1m_{};
};

// Whole-host CPU, memory and load, plus the node's outbound traffic rate. The control loop owns
// the node's only sampler; each sample covers the interval since the previous one.
class runtime_metrics_sampler final {
public:
    // `traffic_out_bytes` is the cumulative outbound byte count of all data workers, empty when
    // it could not be collected; the rate is then 0 and the next collected sample covers the
    // whole interval.
    [[nodiscard]] runtime_metrics_snapshot sample(std::optional<std::uint64_t> traffic_out_bytes) {
        runtime_metrics_snapshot result;
        if (traffic_out_bytes) {
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration<double>(now - last_traffic_sample_).count();
            if (elapsed > 0 && *traffic_out_bytes >= last_traffic_bytes_) {
                result.traffic_out_bps_ = static_cast<std::int64_t>(std::llround(
                    static_cast<double>(*traffic_out_bytes - last_traffic_bytes_) * 8.0 / elapsed));
            }
            last_traffic_bytes_ = *traffic_out_bytes;
            last_traffic_sample_ = now;
        }
        sample_system(result);
        return result;
    }

private:
#ifdef _WIN32
    static std::uint64_t file_time(const FILETIME& value) noexcept {
        ULARGE_INTEGER result;
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    void sample_system(runtime_metrics_snapshot& result) noexcept {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const auto idle_value = file_time(idle);
            const auto total_value = file_time(kernel) + file_time(user);
            if (last_cpu_total_ != 0 && total_value > last_cpu_total_) {
                const auto total_delta = total_value - last_cpu_total_;
                const auto idle_delta = idle_value - last_cpu_idle_;
                result.cpu_usage_ = total_delta > idle_delta
                                        ? static_cast<double>(total_delta - idle_delta) /
                                              static_cast<double>(total_delta)
                                        : 0;
            }
            last_cpu_idle_ = idle_value;
            last_cpu_total_ = total_value;
        }
        MEMORYSTATUSEX memory{.dwLength = sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory)) {
            result.memory_usage_ = static_cast<double>(memory.dwMemoryLoad) / 100.0;
        }
        result.load1m_ = result.cpu_usage_ * (std::max)(1U, std::thread::hardware_concurrency());
    }
#elif defined(__APPLE__)
    void sample_system(runtime_metrics_snapshot& result) noexcept {
        host_cpu_load_info_data_t cpu{};
        mach_msg_type_number_t cpu_count = HOST_CPU_LOAD_INFO_COUNT;
        if (::host_statistics(::mach_host_self(), HOST_CPU_LOAD_INFO,
                reinterpret_cast<host_info_t>(&cpu), &cpu_count) == KERN_SUCCESS) {
            const auto idle_value = static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_IDLE]);
            const auto total_value = static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_USER]) +
                                     static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_SYSTEM]) +
                                     idle_value +
                                     static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_NICE]);
            if (last_cpu_total_ != 0 && total_value > last_cpu_total_) {
                const auto total_delta = total_value - last_cpu_total_;
                const auto idle_delta = idle_value - last_cpu_idle_;
                result.cpu_usage_ = total_delta > idle_delta
                                        ? static_cast<double>(total_delta - idle_delta) /
                                              static_cast<double>(total_delta)
                                        : 0;
            }
            last_cpu_idle_ = idle_value;
            last_cpu_total_ = total_value;
        }

        std::uint64_t total_memory{};
        std::size_t total_memory_size = sizeof(total_memory);
        vm_statistics64_data_t memory{};
        mach_msg_type_number_t memory_count = HOST_VM_INFO64_COUNT;
        vm_size_t page_size{};
        if (::sysctlbyname("hw.memsize", &total_memory, &total_memory_size, nullptr, 0) == 0 &&
            total_memory > 0 &&
            ::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                reinterpret_cast<host_info64_t>(&memory), &memory_count) ==
                KERN_SUCCESS &&
            ::host_page_size(::mach_host_self(), &page_size) == KERN_SUCCESS) {
            const auto used_pages = static_cast<std::uint64_t>(memory.active_count) +
                                    static_cast<std::uint64_t>(memory.inactive_count) +
                                    static_cast<std::uint64_t>(memory.wire_count);
            result.memory_usage_ = (std::min)(1.0, static_cast<double>(used_pages) * static_cast<double>(page_size) /
                                                       static_cast<double>(total_memory));
        }

        double load{};
        if (::getloadavg(&load, 1) == 1 && load >= 0) {
            result.load1m_ = load;
        }
    }
#else
    void sample_system(runtime_metrics_snapshot& result) noexcept {
        std::ifstream stat("/proc/stat");
        std::string label;
        std::uint64_t user{};
        std::uint64_t nice{};
        std::uint64_t system{};
        std::uint64_t idle{};
        std::uint64_t io_wait{};
        std::uint64_t irq{};
        std::uint64_t soft_irq{};
        std::uint64_t steal{};
        if (stat >> label >> user >> nice >> system >> idle >> io_wait >> irq >> soft_irq >> steal) {
            const auto idle_value = idle + io_wait;
            const auto total_value = user + nice + system + idle + io_wait + irq + soft_irq + steal;
            if (last_cpu_total_ != 0 && total_value > last_cpu_total_) {
                const auto total_delta = total_value - last_cpu_total_;
                const auto idle_delta = idle_value - last_cpu_idle_;
                result.cpu_usage_ = total_delta > idle_delta
                                        ? static_cast<double>(total_delta - idle_delta) /
                                              static_cast<double>(total_delta)
                                        : 0;
            }
            last_cpu_idle_ = idle_value;
            last_cpu_total_ = total_value;
        }
        struct sysinfo info{};
        if (::sysinfo(&info) == 0) {
            const auto memory_unit = static_cast<double>(info.mem_unit);
            const auto total = static_cast<double>(info.totalram) * memory_unit;
            const auto free =
                (static_cast<double>(info.freeram) + static_cast<double>(info.bufferram)) *
                memory_unit;
            if (total > 0) {
                result.memory_usage_ = (total - free) / total;
            }
            result.load1m_ = static_cast<double>(info.loads[0]) / (1U << SI_LOAD_SHIFT);
        }
    }
#endif

    std::chrono::steady_clock::time_point last_traffic_sample_{std::chrono::steady_clock::now()};
    std::uint64_t last_traffic_bytes_{};
    std::uint64_t last_cpu_idle_{};
    std::uint64_t last_cpu_total_{};
};

}  // namespace flexedge::node
