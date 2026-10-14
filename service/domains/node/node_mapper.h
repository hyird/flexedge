#pragma once

#include <ruvia/web/context.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <string>

#include "service/common/http.h"
#include "service/domains/node/node_types.h"
#include "service/features/node_config/mapper.h"
#include "service/features/node_runtime/mapper.h"

namespace service::node {

[[noreturn]] inline void throw_corrupt_node_config() {
    service::common::throw_app_error(service::common::server_error_code, "聚合配置损坏", 500);
}

inline std::string serialize_node_config(auto& c,
    const service::node_config::node_config_data& input) {
    const auto output = service::node_config::to_output(input, {.resource_ = c.pool()});
    const auto json = ruvia::to_json(output, {.resource_ = c.pool()});
    return std::string(json.data(), json.size());
}

template <typename row_type>
inline node_runtime_dto get_node_runtime_dto(auto& c,
    const service::node_runtime::node_runtime_data& input,
    const row_type& row) {
    node_runtime_dto output(ruvia::model_options{.resource_ = c.pool()});
    output.set<"registration_status">(row[7].value().value_or("pending"));
    output.set<"connection_status">(row[8].value().value_or("unregistered"));
    output.set<"applied_node_spec_revision">(row[10].template as<std::int64_t>().value_or(0));
    if (const auto& last_heartbeat_at = row[9].value()) {
        output.set<"last_heartbeat_at">(*last_heartbeat_at);
    }
    if (input.agent_version_) {
        output.set<"agent_version">(*input.agent_version_);
    }
    if (input.cpu_usage_) {
        output.set<"cpu_usage">(*input.cpu_usage_);
    }
    if (input.memory_usage_) {
        output.set<"memory_usage">(*input.memory_usage_);
    }
    if (input.traffic_out_bps_) {
        output.set<"traffic_out_bps">(*input.traffic_out_bps_);
    }
    if (input.connection_count_) {
        output.set<"connection_count">(*input.connection_count_);
    }
    if (input.connected_ip_count_) {
        output.set<"connected_ip_count">(*input.connected_ip_count_);
    }
    if (input.load1m_) {
        output.set<"load1m">(*input.load1m_);
    }
    if (input.queued_log_events_) {
        output.set<"queued_log_events">(*input.queued_log_events_);
    }
    if (input.dropped_log_events_) {
        output.set<"dropped_log_events">(*input.dropped_log_events_);
    }
    if (input.health_) {
        output.set<"health">(*input.health_);
    }
    if (input.last_error_) {
        output.set<"last_error">(*input.last_error_);
    }
    if (const auto& active_release_id = row[15].value()) {
        output.set<"active_release_id">(*active_release_id);
    }
    if (const auto& active_manifest_digest = row[16].value()) {
        output.set<"active_manifest_digest">(*active_manifest_digest);
    }
    return output;
}

template <typename row_type>
inline void fill_node(auto& c, node_dto& item, const row_type& row) {
    const auto config = service::node_config::parse_stored(row[5].value().value_or("{}"),
        {.resource_ = c.pool()});
    const auto runtime = service::node_runtime::parse_stored(row[6].value().value_or("{}"),
        {.resource_ = c.pool()});
    if (!config || !runtime) {
        throw_corrupt_node_config();
    }

    item.set<"id">(row[0].value().value_or(""));
    item.set<"cluster_id">(row[1].value().value_or(""));
    item.set<"cluster_name">(row[2].value().value_or(""));
    item.set<"name">(row[13].value().value_or(""));
    item.set<"status">(row[14].value().value_or(""));
    item.set<"revision">(row[3].template as<std::int64_t>().value_or(1));
    item.set<"node_spec_revision">(row[4].template as<std::int64_t>().value_or(1));
    item.set<"config">(service::node_config::to_output(*config, {.resource_ = c.pool()}));
    item.set<"runtime">(get_node_runtime_dto(c, *runtime, row));
    item.set<"created_at">(row[11].value().value_or(""));
    item.set<"updated_at">(row[12].value().value_or(""));
}

}  // namespace service::node
