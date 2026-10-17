#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "service/common/uuid.h"
#include "service/features/node_config/mapper.h"
#include "service/features/node_runtime/transport.h"
#include "service/utils/text.h"

namespace service::node {

RUVIA_MODEL(node_save_input,
    RUVIA_OPTIONAL_FIELD_NAME("cluster_id", cluster_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string),
    RUVIA_OPTIONAL_FIELD(config, service::node_config::node_config_input));

inline void validate_node_save_input(const node_save_input& input, ruvia::validator& validator) {
    const auto& cluster_id = input.get<"cluster_id">();
    validator.required(cluster_id, "cluster_id", "请选择所属集群");
    if (cluster_id && !service::common::parse_uuid(
                          std::optional<std::string_view>{cluster_id->view()})) {
        validator.add("cluster_id", "regex", "所属集群不正确");
    }

    const auto& name = input.get<"name">();
    validator.required(name, "name", "节点名称不能为空");
    validator.min_length(name, "name", 1, "节点名称不能为空");
    validator.max_length(name, "name", 100, "节点名称最多100个字符");
    if (name && !service::utils::has_non_whitespace_ascii(name->view())) {
        validator.add("name", "regex", "节点名称不能为空");
    }

    const auto& status = input.get<"status">();
    validator.required(status, "status", "节点状态不能为空");
    if (status && status->view() != "enabled" && status->view() != "disabled") {
        validator.add("status", "regex", "节点状态不正确");
    }

    const auto& config = input.get<"config">();
    validator.required(config, "config", "节点配置不能为空");
    if (config) {
        service::node_config::validate(*config, validator);
    }
}

struct node_save_data final {
    std::string cluster_id_;
    std::string name_;
    std::string status_;
    service::node_config::node_config_data config_;
};

[[nodiscard]] inline std::optional<node_save_data> normalize(const node_save_input& input) {
    const auto& cluster_id = input.get<"cluster_id">();
    const auto& name = input.get<"name">();
    const auto& status = input.get<"status">();
    const auto& config = input.get<"config">();
    if (!cluster_id || !name || !status || !config) {
        return std::nullopt;
    }
    auto normalized_config = service::node_config::normalize(*config);
    if (!normalized_config) {
        return std::nullopt;
    }
    return node_save_data{.cluster_id_ = std::string(cluster_id->view()),
        .name_ = std::string(name->view()),
        .status_ = std::string(status->view()),
        .config_ = std::move(*normalized_config)};
}

RUVIA_MODEL(node_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("cluster_id", cluster_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("cluster_name", cluster_name, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("node_spec_revision", node_spec_revision,
        ruvia::int64),
    RUVIA_REQUIRED_FIELD(config, service::node_config::node_config_output),
    RUVIA_REQUIRED_FIELD(runtime, service::node_runtime::node_runtime_dto),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));

RUVIA_MODEL(node_page_data_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<node_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));
RUVIA_MODEL(node_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, node_page_data_dto));
RUVIA_MODEL(node_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, node_dto));

RUVIA_MODEL(node_credentials_dto,
    RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_id", node_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(secret, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64));
RUVIA_MODEL(node_credentials_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, node_credentials_dto));

RUVIA_MODEL(node_log_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("occurred_at", occurred_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(level, ruvia::string),
    RUVIA_REQUIRED_FIELD(category, ruvia::string),
    RUVIA_REQUIRED_FIELD(message, ruvia::string));
RUVIA_MODEL(node_log_tail_data_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<node_log_dto>),
    RUVIA_OPTIONAL_FIELD(cursor, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(node_log_tail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, node_log_tail_data_dto));

}  // namespace service::node
