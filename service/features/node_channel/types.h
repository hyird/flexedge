#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::node_channel {

struct agent_principal final {
    std::string node_id_, cluster_id_, tenant_id_, agent_id_;
};
struct desired_summary final {
    std::int64_t node_spec_revision_{};
    std::string release_id_, manifest_digest_;
};
struct heartbeat_report final {
    struct origin_health final {
        std::string website_id_, origin_id_, status_;
        std::int64_t checked_at_unix_millis_{}, latency_millis_{};
        std::string last_error_;
    };
    std::string node_id_;
    std::int64_t applied_node_spec_revision_{};
    std::string active_release_id_, active_manifest_digest_, agent_version_;
    double cpu_usage_{}, memory_usage_{};
    std::optional<std::int64_t> cpu_count_{}, memory_total_bytes_{}, disk_total_bytes_{};
    std::optional<double> disk_usage_{};
    std::int64_t traffic_out_bps_{}, connection_count_{};
    // Absent from nodes before 0.4.1 and when the node could not count.
    std::optional<std::int64_t> connected_ip_count_{};
    double load1m_{};
    std::int64_t queued_log_events_{}, dropped_log_events_{};
    std::string health_, last_error_;
    std::vector<origin_health> origin_health_;
};

struct apply_failure_report final {
    std::int64_t revision_;
    std::string release_id_, manifest_digest_, phase_, error_code_, error_;
    bool retryable_;
};

}  // namespace service::node_channel
