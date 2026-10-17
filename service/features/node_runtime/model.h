#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::node_runtime {

struct node_runtime_data final {
    struct origin_health final {
        std::string website_id_;
        std::string origin_id_;
        std::string status_;
        std::int64_t checked_at_unix_millis_{};
        std::int64_t latency_millis_{};
        std::optional<std::string> last_error_;
    };
    std::optional<std::string> agent_version_;
    std::optional<double> cpu_usage_;
    std::optional<double> memory_usage_;
    std::optional<std::int64_t> cpu_count_;
    std::optional<std::int64_t> memory_total_bytes_;
    std::optional<std::int64_t> disk_total_bytes_;
    std::optional<std::int64_t> traffic_out_bps_;
    std::optional<std::int64_t> connection_count_;
    std::optional<std::int64_t> connected_ip_count_;
    std::optional<double> load1m_;
    std::optional<std::int64_t> queued_log_events_;
    std::optional<std::int64_t> dropped_log_events_;
    std::vector<origin_health> origin_health_;
    std::optional<std::string> health_;
    std::optional<std::string> last_error_;
};

}  // namespace service::node_runtime
