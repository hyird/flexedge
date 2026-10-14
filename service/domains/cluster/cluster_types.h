#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"

#include "service/common/uuid.h"

namespace service::cluster {

inline bool has_non_whitespace(const ruvia::string& value) {
    return value.view().find_first_not_of(" \\t\\r\\n\\f\\v") != std::string_view::npos;
}

inline bool is_valid_uuid(const ruvia::string& value) {
    return service::common::parse_uuid(std::optional<std::string_view>{value.view()}).has_value();
}

inline bool is_valid_hostname_prefix(const ruvia::string& value) {
    const auto text = value.view();
    if (text.empty() || text.size() > 63 ||
        !((text.front() >= 'A' && text.front() <= 'Z') ||
            (text.front() >= 'a' && text.front() <= 'z') ||
            (text.front() >= '0' && text.front() <= '9'))) {
        return false;
    }
    for (const char ch : text) {
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '-')) {
            return false;
        }
    }
    return (text.back() >= 'A' && text.back() <= 'Z') ||
           (text.back() >= 'a' && text.back() <= 'z') ||
           (text.back() >= '0' && text.back() <= '9');
}

RUVIA_MODEL(save_cluster_body, RUVIA_OPTIONAL_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("dns_zone_id", dns_zone_id, ruvia::string),
    RUVIA_OPTIONAL_FIELD_NAME("hostname_prefix", hostname_prefix, ruvia::string),
    RUVIA_OPTIONAL_FIELD(status, ruvia::string));

struct cluster_save_data final {
    std::string name_;
    std::string dns_zone_id_;
    std::string hostname_prefix_;
    std::string status_;
};

[[nodiscard]] inline std::optional<cluster_save_data> normalize(const save_cluster_body& input) {
    const auto& name = input.get<"name">();
    const auto& dns_zone_id = input.get<"dns_zone_id">();
    const auto& hostname_prefix = input.get<"hostname_prefix">();
    const auto& status = input.get<"status">();
    if (!name || !dns_zone_id || !hostname_prefix || !status) {
        return std::nullopt;
    }
    return cluster_save_data{.name_ = std::string(name->view()),
        .dns_zone_id_ = std::string(dns_zone_id->view()),
        .hostname_prefix_ = std::string(hostname_prefix->view()),
        .status_ = std::string(status->view())};
}

RUVIA_MODEL(cluster_dto, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_id", dns_zone_id, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_zone_domain", dns_zone_domain, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("dns_provider_name", dns_provider_name, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("hostname_prefix", hostname_prefix, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("access_domain", access_domain, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("node_count", node_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("online_node_count", online_node_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(status, ruvia::string),
    RUVIA_REQUIRED_FIELD(revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("created_at", created_at, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("updated_at", updated_at, ruvia::string));

RUVIA_MODEL(cluster_page_data_dto, RUVIA_REQUIRED_FIELD(list, ruvia::array<cluster_dto>),
    RUVIA_REQUIRED_FIELD(total, ruvia::int64),
    RUVIA_REQUIRED_FIELD(page, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("page_size", page_size, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("total_pages", total_pages, ruvia::int64));

RUVIA_MODEL(cluster_page_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, cluster_page_data_dto));
RUVIA_MODEL(cluster_detail_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, cluster_dto));
RUVIA_MODEL(cluster_options_response, RUVIA_REQUIRED_FIELD(code, ruvia::int64),
    RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(data, ruvia::array<cluster_dto>));

}  // namespace service::cluster
