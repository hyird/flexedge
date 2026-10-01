#pragma once

#include "ruvia/web/Model.h"
#include "ruvia/web/Validation.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "service/common/uuid.h"
#include "service/features/node_config/mapper.h"

namespace service::node {

RUVIA_MODEL(NodeSaveInput,
    RUVIA_OPTIONAL_FIELD_NAME("cluster_id", clusterId, ruvia::String),
    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
    RUVIA_OPTIONAL_FIELD(status, ruvia::String),
    RUVIA_OPTIONAL_FIELD(config, service::node_config::NodeConfigInput));

[[nodiscard]] inline bool hasNonWhitespaceName(std::string_view value) {
    return value.find_first_not_of(" \t\n\r\f\v") != std::string_view::npos;
}

inline void validateNodeSaveInput(const NodeSaveInput& input, ruvia::Validator& validator) {
    const auto& clusterId = input.get<"clusterId">();
    validator.required(clusterId, "cluster_id", "请选择所属集群");
    if (clusterId && !service::common::parseUuid(
                         std::optional<std::string_view>{clusterId->view()})) {
        validator.add("cluster_id", "regex", "所属集群不正确");
    }

    const auto& name = input.get<"name">();
    validator.required(name, "name", "节点名称不能为空");
    validator.minLength(name, "name", 1, "节点名称不能为空");
    validator.maxLength(name, "name", 100, "节点名称最多100个字符");
    if (name && !hasNonWhitespaceName(name->view())) {
        validator.add("name", "regex", "节点名称不能为空");
    }

    const auto& status = input.get<"status">();
    validator.required(status, "status", "节点状态不能为空");
    if (status && status->view() != "enabled" && status->view() != "disabled") {
        validator.add("status", "regex", "节点状态不正确");
    }

    const auto& config = input.get<"config">();
    validator.required(config, "config", "节点配置不能为空");
    if (config) service::node_config::validate(*config, validator);
}

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
    if (!clusterId || !name || !status || !config) return std::nullopt;
    auto normalizedConfig = service::node_config::normalize(*config);
    if (!normalizedConfig) {
        return std::nullopt;
    }
    return NodeSaveData{.clusterId = std::string(clusterId->view()),
                        .name = std::string(name->view()),
                        .status = std::string(status->view()),
                        .config = std::move(*normalizedConfig)};
}

RUVIA_MODEL(
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

RUVIA_MODEL(NodeDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
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

RUVIA_MODEL(NodePageDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<NodeDto>),
                     RUVIA_REQUIRED_FIELD(total, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(page, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("page_size", pageSize, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("total_pages", totalPages, ruvia::Int64));
RUVIA_MODEL(NodePageResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodePageDataDto));

RUVIA_MODEL(NodeCredentialsDto,
                     RUVIA_REQUIRED_FIELD_NAME("node_id", nodeId, ruvia::String),
                     RUVIA_REQUIRED_FIELD(secret, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64));
RUVIA_MODEL(NodeCredentialsResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodeCredentialsDto));

RUVIA_MODEL(NodeLogDto, RUVIA_REQUIRED_FIELD(id, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("occurred_at", occurredAt, ruvia::String),
                     RUVIA_REQUIRED_FIELD(level, ruvia::String),
                     RUVIA_REQUIRED_FIELD(category, ruvia::String),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String));
RUVIA_MODEL(NodeLogTailDataDto, RUVIA_REQUIRED_FIELD(list, ruvia::Array<NodeLogDto>),
                     RUVIA_OPTIONAL_FIELD(cursor, ruvia::String, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(NodeLogTailResponse, RUVIA_REQUIRED_FIELD(code, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(message, ruvia::String),
                     RUVIA_REQUIRED_FIELD(data, NodeLogTailDataDto));

} // namespace service::node
