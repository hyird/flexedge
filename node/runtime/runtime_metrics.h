#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <mutex>
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

struct RuntimeMetricsSnapshot final {
    double cpuUsage{};
    double memoryUsage{};
    std::int64_t trafficOutBps{};
    std::int64_t connectionCount{};
    double load1m{};
};

class RuntimeMetrics final {
  public:
    void connectionOpened() noexcept { activeConnections_.fetch_add(1); }
    void connectionClosed() noexcept { activeConnections_.fetch_sub(1); }
    void trafficOut(std::size_t bytes) noexcept { trafficOutBytes_.fetch_add(bytes); }

    [[nodiscard]] std::int64_t connectionCount() const noexcept {
        return activeConnections_.load();
    }

    [[nodiscard]] RuntimeMetricsSnapshot sample() {
        const std::lock_guard lock(sampleMutex_);
        RuntimeMetricsSnapshot result;
        result.connectionCount = activeConnections_.load();
        const auto now = std::chrono::steady_clock::now();
        const auto bytes = trafficOutBytes_.load();
        const auto elapsed = std::chrono::duration<double>(now - lastTrafficSample_).count();
        if (elapsed > 0) {
            result.trafficOutBps = static_cast<std::int64_t>(
                std::llround(static_cast<double>(bytes - lastTrafficBytes_) * 8.0 / elapsed));
        }
        lastTrafficBytes_ = bytes;
        lastTrafficSample_ = now;
        sampleSystem(result);
        return result;
    }

  private:
#ifdef _WIN32
    static std::uint64_t fileTime(const FILETIME& value) noexcept {
        ULARGE_INTEGER result;
        result.LowPart = value.dwLowDateTime;
        result.HighPart = value.dwHighDateTime;
        return result.QuadPart;
    }

    void sampleSystem(RuntimeMetricsSnapshot& result) noexcept {
        FILETIME idle{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const auto idleValue = fileTime(idle);
            const auto totalValue = fileTime(kernel) + fileTime(user);
            if (lastCpuTotal_ != 0 && totalValue > lastCpuTotal_) {
                const auto totalDelta = totalValue - lastCpuTotal_;
                const auto idleDelta = idleValue - lastCpuIdle_;
                result.cpuUsage = totalDelta > idleDelta
                                      ? static_cast<double>(totalDelta - idleDelta) /
                                            static_cast<double>(totalDelta)
                                      : 0;
            }
            lastCpuIdle_ = idleValue;
            lastCpuTotal_ = totalValue;
        }
        MEMORYSTATUSEX memory{.dwLength = sizeof(memory)};
        if (GlobalMemoryStatusEx(&memory)) {
            result.memoryUsage = static_cast<double>(memory.dwMemoryLoad) / 100.0;
        }
        result.load1m = result.cpuUsage * (std::max)(1U, std::thread::hardware_concurrency());
    }
#elif defined(__APPLE__)
    void sampleSystem(RuntimeMetricsSnapshot& result) noexcept {
        host_cpu_load_info_data_t cpu{};
        mach_msg_type_number_t cpuCount = HOST_CPU_LOAD_INFO_COUNT;
        if (::host_statistics(::mach_host_self(), HOST_CPU_LOAD_INFO,
                              reinterpret_cast<host_info_t>(&cpu), &cpuCount) == KERN_SUCCESS) {
            const auto idleValue = static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_IDLE]);
            const auto totalValue = static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_USER]) +
                                    static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_SYSTEM]) +
                                    idleValue +
                                    static_cast<std::uint64_t>(cpu.cpu_ticks[CPU_STATE_NICE]);
            if (lastCpuTotal_ != 0 && totalValue > lastCpuTotal_) {
                const auto totalDelta = totalValue - lastCpuTotal_;
                const auto idleDelta = idleValue - lastCpuIdle_;
                result.cpuUsage = totalDelta > idleDelta
                                      ? static_cast<double>(totalDelta - idleDelta) /
                                            static_cast<double>(totalDelta)
                                      : 0;
            }
            lastCpuIdle_ = idleValue;
            lastCpuTotal_ = totalValue;
        }

        std::uint64_t totalMemory{};
        std::size_t totalMemorySize = sizeof(totalMemory);
        vm_statistics64_data_t memory{};
        mach_msg_type_number_t memoryCount = HOST_VM_INFO64_COUNT;
        vm_size_t pageSize{};
        if (::sysctlbyname("hw.memsize", &totalMemory, &totalMemorySize, nullptr, 0) == 0 &&
            totalMemory > 0 &&
            ::host_statistics64(::mach_host_self(), HOST_VM_INFO64,
                                reinterpret_cast<host_info64_t>(&memory), &memoryCount) ==
                KERN_SUCCESS &&
            ::host_page_size(::mach_host_self(), &pageSize) == KERN_SUCCESS) {
            const auto usedPages = static_cast<std::uint64_t>(memory.active_count) +
                                   static_cast<std::uint64_t>(memory.inactive_count) +
                                   static_cast<std::uint64_t>(memory.wire_count);
            result.memoryUsage = (std::min)(
                1.0, static_cast<double>(usedPages) * static_cast<double>(pageSize) /
                         static_cast<double>(totalMemory));
        }

        double load{};
        if (::getloadavg(&load, 1) == 1 && load >= 0) {
            result.load1m = load;
        }
    }
#else
    void sampleSystem(RuntimeMetricsSnapshot& result) noexcept {
        std::ifstream stat("/proc/stat");
        std::string label;
        std::uint64_t user{};
        std::uint64_t nice{};
        std::uint64_t system{};
        std::uint64_t idle{};
        std::uint64_t ioWait{};
        std::uint64_t irq{};
        std::uint64_t softIrq{};
        std::uint64_t steal{};
        if (stat >> label >> user >> nice >> system >> idle >> ioWait >> irq >> softIrq >> steal) {
            const auto idleValue = idle + ioWait;
            const auto totalValue = user + nice + system + idle + ioWait + irq + softIrq + steal;
            if (lastCpuTotal_ != 0 && totalValue > lastCpuTotal_) {
                const auto totalDelta = totalValue - lastCpuTotal_;
                const auto idleDelta = idleValue - lastCpuIdle_;
                result.cpuUsage = totalDelta > idleDelta
                                      ? static_cast<double>(totalDelta - idleDelta) /
                                            static_cast<double>(totalDelta)
                                      : 0;
            }
            lastCpuIdle_ = idleValue;
            lastCpuTotal_ = totalValue;
        }
        struct sysinfo info{};
        if (::sysinfo(&info) == 0) {
            const auto memoryUnit = static_cast<double>(info.mem_unit);
            const auto total = static_cast<double>(info.totalram) * memoryUnit;
            const auto free =
                (static_cast<double>(info.freeram) + static_cast<double>(info.bufferram)) *
                memoryUnit;
            if (total > 0) {
                result.memoryUsage = (total - free) / total;
            }
            result.load1m = static_cast<double>(info.loads[0]) / (1U << SI_LOAD_SHIFT);
        }
    }
#endif

    std::atomic<std::int64_t> activeConnections_{};
    std::atomic<std::uint64_t> trafficOutBytes_{};
    std::mutex sampleMutex_;
    std::chrono::steady_clock::time_point lastTrafficSample_{std::chrono::steady_clock::now()};
    std::uint64_t lastTrafficBytes_{};
    std::uint64_t lastCpuIdle_{};
    std::uint64_t lastCpuTotal_{};
};

} // namespace flexedge::node
