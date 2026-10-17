#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace service::website_config {

struct website_domain_data final {
    std::string id_;
    std::string hostname_;
    std::string dns_mode_;
};

struct website_origin_data final {
    std::string id_;
    std::string group_;
    std::string protocol_;
    std::string host_;
    std::int64_t port_;
    std::string role_;
    std::int64_t weight_;
    std::string status_;
};

struct website_route_header_data final {
    std::string name_;
    std::string value_;
};

struct website_route_condition_data final {
    std::string source_;
    std::string name_;
    std::string op_;
    std::string value_;
};

struct website_route_rule_data final {
    std::string id_;
    std::string name_;
    std::string description_;
    std::vector<std::string> hostnames_;
    std::vector<website_route_condition_data> conditions_;
    std::string rewrite_mode_;
    std::string query_mode_;
    std::string query_string_;
    std::string status_;
    std::string match_type_;
    std::string path_;
    std::vector<std::string> methods_;
    std::string action_;
    std::string rewrite_path_;
    std::string redirect_url_;
    std::int64_t redirect_status_;
    std::string origin_group_;
    std::vector<website_route_header_data> request_headers_;
    std::vector<website_route_header_data> response_headers_;
};

struct website_config_data final {
    bool cache_enabled_{false};
    std::string cache_policy_id_{};
    std::optional<std::string> name_;
    std::vector<website_domain_data> domains_;
    std::vector<website_origin_data> origins_;
    std::string default_origin_group_;
    std::string origin_host_header_;
    std::string origin_http_version_{"auto"};
    std::int64_t origin_connect_timeout_seconds_;
    std::int64_t origin_read_timeout_seconds_;
    bool pass_client_ip_;
    bool health_check_enabled_;
    std::string health_check_path_;
    std::int64_t health_check_interval_seconds_;
    std::int64_t health_check_timeout_seconds_;
    std::int64_t health_check_expected_status_;
    std::int64_t healthy_threshold_;
    std::int64_t unhealthy_threshold_;
    bool access_log_enabled_;
    bool access_log_request_headers_;
    bool access_log_request_body_;
    bool access_log_response_headers_;
    bool access_log_query_params_;
    bool access_log_cookies_;
    bool access_log_referer_;
    bool access_log_user_agent_;
    std::vector<std::string> access_log_status_code_ranges_;
    bool access_log_client_abort_;
    bool https_enabled_;
    std::vector<std::string> certificate_ids_;
    std::string minimum_tls_version_;
    bool force_https_;
    bool hsts_enabled_;
    bool response_compression_enabled_;
    std::int64_t response_compression_min_bytes_;
    std::int64_t response_compression_max_bytes_;
    std::vector<std::string> response_compression_algorithms_;
    std::vector<std::string> response_compression_mime_types_;
    std::vector<std::string> response_compression_extensions_;
    std::vector<std::string> response_compression_excluded_extensions_;
    std::vector<website_route_rule_data> route_rules_;
};

}  // namespace service::website_config
