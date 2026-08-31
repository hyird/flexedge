#pragma once

#include <optional>
#include <string>
#include <utility>

#include <ruvia/web/Model.h>

#include "service/features/node_config/model.h"

namespace service::node {

RUVIA_REQUEST_MODEL(NodeSaveInput,
                    RUVIA_OPTIONAL_FIELD_NAME("cluster_id", clusterId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(config, service::node_config::NodeConfigInput));

struct NodeSaveData final {
    std::string clusterId;
    std::string name;
    std::string status;
    service::node_config::NodeConfigData config;
};

[[nodiscard]] inline std::optional<NodeSaveData> normalize(const NodeSaveInput& input) {
    const auto& clusterId = input.get<"clusterId">();
    const auto& name = input.get<"name">();
    const auto& status = input.get<"status">();
    const auto& config = input.get<"config">();
    if (!clusterId || !name || !status || !config) {
        return std::nullopt;
    }
    auto normalizedConfig = service::node_config::normalize(*config);
    if (!normalizedConfig) {
        return std::nullopt;
    }
    return NodeSaveData{.clusterId = std::string(clusterId->view()),
                        .name = std::string(name->view()),
                        .status = std::string(status->view()),
                        .config = std::move(*normalizedConfig)};
}

RUVIA_RESPONSE_MODEL(
    NodeRuntimeDto,
    RUVIA_REQUIRED_FIELD_NAME("registration_status", registrationStatus, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("connection_status", connectionStatus, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_heartbeat_at", lastHeartbeatAt, ruvia::String,
                              RUVIA_OMIT_EMPTY),
    RUVIA_REQUIRED_FIELD_NAME("applied_node_spec_revision", appliedNodeSpecRevision, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("active_release_id", activeReleaseId, ruvia::String,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("active_manifest_digest", activeManifestDigest, ruvia::String,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("agent_version", agentVersion, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("cpu_usage", cpuUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("memory_usage", memoryUsage, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("traffic_out_bps", trafficOutBps, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("connection_count", connectionCount, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("load_1m", load1m, ruvia::Double, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("queued_log_events", queuedLogEvents, ruvia::Int64, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("dropped_log_events", droppedLogEvents, ruvia::Int64,
                              RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(health, ruvia::String, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD_NAME("last_error", lastError, ruvia::String, RUVIA_OMIT_EMPTY));

RUVIA_RESPONSE_MODEL(NodeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("cluster_id", clusterId, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("cluster_name", clusterName, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD(status, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("node_spec_revision", nodeSpecRevision,
                                               ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(config, service::node_config::NodeConfigOutput),
                     RUVIA_REQUIRED_FIELD(runtime, NodeRuntimeDto),
                     RUVIA_REQUIRED_FIELD_NAME("created_at", createdAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("updated_at", updatedAt, ruvia::String));

RUVIA_RESPONSE_MODEL(NodePageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<NodeDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_RESPONSE_MODEL(NodePageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodePageDataDto));

RUVIA_RESPONSE_MODEL(NodeCredentialsDto,
                     RUVIA_REQUIRED_FIELD_NAME("node_id", nodeId, ruvia::String),
                     RUVIA_REQUIRED_FIELD(secret, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64));
RUVIA_RESPONSE_MODEL(NodeCredentialsResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodeCredentialsDto));

RUVIA_RESPONSE_MODEL(NodeLogDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("occurred_at", occurredAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD(level, ruvia::String),
                     RUVIA_REQUIRED_FIELD(category, ruvia::String),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String));
RUVIA_RESPONSE_MODEL(NodeLogTailDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<NodeLogDto>),
                     RUVIA_OPTIONAL_FIELD(cursor, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_RESPONSE_MODEL(NodeLogTailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodeLogTailDataDto));

} // namespace service::node
