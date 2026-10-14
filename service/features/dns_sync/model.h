#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::dns_sync {

struct zone_record_data final {
    std::string id_;
    std::string type_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::optional<std::int64_t> priority_;
    bool proxied_;
    std::string line_code_;
};

struct zone_config_data final {
    std::vector<zone_record_data> records_;
};

struct zone_line_runtime_data final {
    std::optional<std::string> code_;
    std::optional<std::string> name_;
    std::optional<std::string> display_name_;
    std::optional<std::string> status_;
};

struct zone_record_runtime_data final {
    std::optional<std::string> id_;
    std::optional<std::string> remote_record_id_;
    std::optional<std::string> sync_status_;
    std::optional<std::int64_t> synced_revision_;
    std::optional<std::string> last_error_;
};

struct zone_record_conflict_data final {
    std::optional<std::string> id_;
    std::optional<std::string> type_;
    std::optional<std::string> name_;
    std::optional<std::string> line_code_;
    std::optional<std::string> local_content_;
    std::optional<std::string> remote_content_;
};

struct zone_challenge_runtime_data final {
    std::string id_;
    std::string name_;
    std::string content_;
    std::int64_t ttl_;
    std::string certificate_id_;
};

struct zone_runtime_data final {
    bool records_imported_{false};
    std::optional<std::string> lines_synced_at_;
    std::vector<zone_line_runtime_data> lines_;
    std::vector<zone_record_runtime_data> record_states_;
    std::vector<zone_record_conflict_data> conflicts_;
    std::vector<zone_challenge_runtime_data> challenge_records_;
};

}  // namespace service::dns_sync
