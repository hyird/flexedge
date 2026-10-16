#pragma once

#include <ruvia/web/model.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Wire shape and bounds of one attempt trace.
// The trace is diagnostic data stored next to the attempt record; it never
// holds configuration versions, credentials or key material.
namespace service::attempt_trace {

inline constexpr std::size_t trace_max_bytes{262144};
inline constexpr std::size_t max_steps{128};
inline constexpr std::size_t max_calls{200};
inline constexpr std::size_t max_step_calls{100};
inline constexpr std::size_t max_logs{300};
inline constexpr std::size_t max_log_bytes{49152};
inline constexpr std::size_t log_message_bytes{1024};
inline constexpr std::size_t request_excerpt_bytes{2048};
inline constexpr std::size_t response_excerpt_bytes{8192};
inline constexpr std::size_t body_budget_bytes{163840};
inline constexpr std::size_t parse_body_max_bytes{524288};
inline constexpr std::size_t url_bytes{2048};
inline constexpr std::size_t value_bytes{256};
inline constexpr std::size_t error_bytes{1000};
inline constexpr std::size_t identifier_bytes{128};
inline constexpr std::size_t max_headers{32};
inline constexpr std::size_t max_query_parameters{32};
inline constexpr std::size_t max_dns_answers{16};
inline constexpr std::size_t max_json_items{50};
inline constexpr std::size_t summary_max_bytes{2048};
inline constexpr std::size_t summary_steps{24};
// Full traces kept per task, resource and version: the newest ones plus the
// first retained attempt. Older attempts keep their history row only.
inline constexpr std::int64_t retained_traces{20};
inline constexpr std::string_view redacted_marker{"[REDACTED]"};

enum class trace_kind : std::uint8_t {
    dns_sync,
    certificate,
    website_domains,
    provider,
    node_apply,
};

[[nodiscard]] inline constexpr std::string_view trace_kind_name(trace_kind kind) noexcept {
    switch (kind) {
        case trace_kind::dns_sync:
            return "dns_sync";
        case trace_kind::certificate:
            return "certificate";
        case trace_kind::website_domains:
            return "website_domains";
        case trace_kind::provider:
            return "provider";
        case trace_kind::node_apply:
            return "node_apply";
    }
    return "none";
}

// Stable step codes with the Chinese label written into the trace.
enum class step_code : std::uint8_t {
    load_state,
    open_credential,
    validate_ttl,
    find_zone,
    list_records,
    list_lines,
    delete_obsolete,
    reconcile_records,
    persist_result,
    prepare,
    account,
    order,
    authorization,
    dns_challenge,
    validation,
    cleanup,
    finalize,
    download,
    persist,
    load_website,
    check_domain,
    save_runtime,
    load_provider,
    decrypt_credential,
    verify_dns_credential,
    load_acme_settings,
    load_eab,
    check_acme_directory,
    save_result,
    send_desired_state,
    serve_objects,
    load_objects,
    fetch_objects,
    stage_config,
    validate_config,
    prepare_listeners,
    activate_config,
    node_step,
    record_result,
    record_failure,
    unscoped,
};

struct step_descriptor final {
    step_code code_;
    std::string_view name_;
    std::string_view label_;
};

inline constexpr std::array step_descriptors{
    step_descriptor{step_code::load_state, "load_state", "加载同步状态"},
    step_descriptor{step_code::open_credential, "open_credential", "解密凭据"},
    step_descriptor{step_code::validate_ttl, "validate_ttl", "校验 TTL"},
    step_descriptor{step_code::find_zone, "find_zone", "查找远程托管域名"},
    step_descriptor{step_code::list_records, "list_records", "拉取远程记录"},
    step_descriptor{step_code::list_lines, "list_lines", "拉取解析线路"},
    step_descriptor{step_code::delete_obsolete, "delete_obsolete", "删除过期远程记录"},
    step_descriptor{step_code::reconcile_records, "reconcile_records", "同步期望记录"},
    step_descriptor{step_code::persist_result, "persist_result", "保存同步结果"},
    step_descriptor{step_code::prepare, "prepare", "准备签发"},
    step_descriptor{step_code::account, "account", "ACME 账户"},
    step_descriptor{step_code::order, "order", "创建订单"},
    step_descriptor{step_code::authorization, "authorization", "获取域名授权"},
    step_descriptor{step_code::dns_challenge, "dns_challenge", "写入 DNS 验证记录并等待生效"},
    step_descriptor{step_code::validation, "validation", "证书颁发机构验证"},
    step_descriptor{step_code::cleanup, "cleanup", "清理 DNS 验证记录"},
    step_descriptor{step_code::finalize, "finalize", "提交证书请求并等待签发"},
    step_descriptor{step_code::download, "download", "下载并校验证书"},
    step_descriptor{step_code::persist, "persist", "保存证书"},
    step_descriptor{step_code::load_website, "load_website", "读取网站配置"},
    step_descriptor{step_code::check_domain, "check_domain", "检测域名解析"},
    step_descriptor{step_code::save_runtime, "save_runtime", "保存解析结果"},
    step_descriptor{step_code::load_provider, "load_provider", "读取供应商状态"},
    step_descriptor{step_code::decrypt_credential, "decrypt_credential", "解密凭据"},
    step_descriptor{step_code::verify_dns_credential, "verify_dns_credential", "校验 DNS 服务商凭据"},
    step_descriptor{step_code::load_acme_settings, "load_acme_settings", "解析证书供应商配置"},
    step_descriptor{step_code::load_eab, "load_eab", "获取 EAB 凭据"},
    step_descriptor{step_code::check_acme_directory, "check_acme_directory", "检测 ACME 目录"},
    step_descriptor{step_code::save_result, "save_result", "保存检测结果"},
    step_descriptor{step_code::send_desired_state, "send_desired_state", "下发期望状态"},
    step_descriptor{step_code::serve_objects, "serve_objects", "发送发布对象"},
    step_descriptor{step_code::load_objects, "load_objects", "读取本地缓存对象"},
    step_descriptor{step_code::fetch_objects, "fetch_objects", "向控制面请求缺失对象"},
    step_descriptor{step_code::stage_config, "stage_config", "暂存配置"},
    step_descriptor{step_code::validate_config, "validate_config", "校验配置"},
    step_descriptor{step_code::prepare_listeners, "prepare_listeners", "准备监听"},
    step_descriptor{step_code::activate_config, "activate_config", "激活配置"},
    step_descriptor{step_code::node_step, "node_step", "节点步骤"},
    step_descriptor{step_code::record_result, "record_result", "记录应用结果"},
    step_descriptor{step_code::record_failure, "record_failure", "记录失败"},
    step_descriptor{step_code::unscoped, "unscoped", "其他操作"},
};

// Node apply phases reported in ApplyResult.steps, mapped to step codes.
[[nodiscard]] inline constexpr step_code node_phase_step(std::string_view phase) noexcept {
    if (phase == "load_objects") {
        return step_code::load_objects;
    }
    if (phase == "fetch_objects") {
        return step_code::fetch_objects;
    }
    if (phase == "stage") {
        return step_code::stage_config;
    }
    if (phase == "validate") {
        return step_code::validate_config;
    }
    if (phase == "prepare_listeners") {
        return step_code::prepare_listeners;
    }
    if (phase == "activate") {
        return step_code::activate_config;
    }
    return step_code::node_step;
}

[[nodiscard]] inline constexpr const step_descriptor& describe_step(step_code code) noexcept {
    for (const auto& descriptor : step_descriptors) {
        if (descriptor.code_ == code) {
            return descriptor;
        }
    }
    return step_descriptors.back();
}

enum class step_outcome : std::uint8_t {
    completed,
    failed,
    skipped,
    interrupted,
};

[[nodiscard]] inline constexpr std::string_view step_outcome_name(step_outcome outcome) noexcept {
    switch (outcome) {
        case step_outcome::completed:
            return "completed";
        case step_outcome::failed:
            return "failed";
        case step_outcome::skipped:
            return "skipped";
        case step_outcome::interrupted:
            return "interrupted";
    }
    return "interrupted";
}

// Severity order used when adjacent summary steps merge: the worst wins.
[[nodiscard]] inline constexpr int step_outcome_severity(step_outcome outcome) noexcept {
    switch (outcome) {
        case step_outcome::completed:
            return 0;
        case step_outcome::skipped:
            return 1;
        case step_outcome::interrupted:
            return 2;
        case step_outcome::failed:
            return 3;
    }
    return 3;
}

enum class log_level : std::uint8_t {
    info,
    warning,
    error,
};

[[nodiscard]] inline constexpr std::string_view log_level_name(log_level level) noexcept {
    switch (level) {
        case log_level::info:
            return "info";
        case log_level::warning:
            return "warning";
        case log_level::error:
            return "error";
    }
    return "info";
}

RUVIA_MODEL(trace_pair_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(value, ruvia::string),
    RUVIA_REQUIRED_FIELD(redacted, ruvia::bool_value));
RUVIA_MODEL(trace_body_dto, RUVIA_REQUIRED_FIELD(content_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(format, ruvia::string), RUVIA_REQUIRED_FIELD(bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(excerpt, ruvia::string), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(omitted_reason, ruvia::string));
RUVIA_MODEL(trace_http_dto, RUVIA_REQUIRED_FIELD(method, ruvia::string), RUVIA_REQUIRED_FIELD(url, ruvia::string),
    RUVIA_REQUIRED_FIELD(scheme, ruvia::string), RUVIA_REQUIRED_FIELD(host, ruvia::string),
    RUVIA_REQUIRED_FIELD(path, ruvia::string), RUVIA_REQUIRED_FIELD(query, ruvia::array<trace_pair_dto>),
    RUVIA_REQUIRED_FIELD(request_headers, ruvia::array<trace_pair_dto>),
    RUVIA_REQUIRED_FIELD(request_body, trace_body_dto, RUVIA_NULLABLE), RUVIA_REQUIRED_FIELD(status, ruvia::int64),
    RUVIA_REQUIRED_FIELD(transport_error, ruvia::string),
    RUVIA_REQUIRED_FIELD(response_headers, ruvia::array<trace_pair_dto>),
    RUVIA_REQUIRED_FIELD(response_body, trace_body_dto, RUVIA_NULLABLE));
RUVIA_MODEL(trace_dns_answer_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64), RUVIA_REQUIRED_FIELD(data, ruvia::string));
RUVIA_MODEL(trace_dns_dto, RUVIA_REQUIRED_FIELD(resolver, ruvia::string),
    RUVIA_REQUIRED_FIELD(transport, ruvia::string), RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(type, ruvia::string), RUVIA_REQUIRED_FIELD(rcode, ruvia::string),
    RUVIA_REQUIRED_FIELD(answers, ruvia::array<trace_dns_answer_dto>),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value));
RUVIA_MODEL(trace_node_field_dto, RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(value, ruvia::string));
RUVIA_MODEL(trace_node_message_dto, RUVIA_REQUIRED_FIELD(direction, ruvia::string),
    RUVIA_REQUIRED_FIELD(message, ruvia::string), RUVIA_REQUIRED_FIELD(request_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(object_count, ruvia::int64), RUVIA_REQUIRED_FIELD(bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(fields, ruvia::array<trace_node_field_dto>));
RUVIA_MODEL(trace_call_dto, RUVIA_REQUIRED_FIELD(index, ruvia::int64), RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(operation, ruvia::string), RUVIA_REQUIRED_FIELD(subject, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64), RUVIA_REQUIRED_FIELD(repeat_total_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(last_started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider_request_id, ruvia::string), RUVIA_REQUIRED_FIELD(provider_code, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(http, trace_http_dto, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(dns, trace_dns_dto, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(node_message, trace_node_message_dto, RUVIA_NULLABLE));
RUVIA_MODEL(trace_log_dto, RUVIA_REQUIRED_FIELD(at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(level, ruvia::string), RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64));
RUVIA_MODEL(trace_related_task_dto, RUVIA_REQUIRED_FIELD(resource_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(resource_id, ruvia::string), RUVIA_REQUIRED_FIELD(label, ruvia::string));
RUVIA_MODEL(trace_step_dto, RUVIA_REQUIRED_FIELD(index, ruvia::int64), RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(label, ruvia::string), RUVIA_REQUIRED_FIELD(subject, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(related_task, trace_related_task_dto, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(calls, ruvia::array<trace_call_dto>), RUVIA_REQUIRED_FIELD(logs, ruvia::array<trace_log_dto>));
RUVIA_MODEL(trace_capture_dto, RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(degraded, ruvia::bool_value), RUVIA_REQUIRED_FIELD(bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_steps, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_logs, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_body_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(redactions, ruvia::int64));
// Stored in sys_sync_event_trace.trace. The attempt fields (number, outcome,
// error, timing) are joined from sys_sync_event when the trace is read.
RUVIA_MODEL(trace_document_dto, RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(worker_type, ruvia::string), RUVIA_REQUIRED_FIELD(capture, trace_capture_dto),
    RUVIA_REQUIRED_FIELD(steps, ruvia::array<trace_step_dto>));

RUVIA_MODEL(trace_step_summary_dto, RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(label, ruvia::string), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(call_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(count, ruvia::int64));
RUVIA_MODEL(trace_failed_step_dto, RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(label, ruvia::string));
// Stored in sys_sync_event_trace.summary; the history read adds attempt_id.
RUVIA_MODEL(trace_summary_dto, RUVIA_REQUIRED_FIELD(state, ruvia::string),
    RUVIA_REQUIRED_FIELD(step_count, ruvia::int64), RUVIA_REQUIRED_FIELD(call_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(log_count, ruvia::int64), RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(failed_step, trace_failed_step_dto, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(steps, ruvia::array<trace_step_summary_dto>));

// Summary of an attempt without a stored trace (before 0024, node attempts
// and traces that could not be stored).
inline constexpr std::string_view summary_none_json{
    R"({"state":"none","step_count":0,"call_count":0,"log_count":0,"truncated":false,"failed_step":null,"steps":[]})"};
inline constexpr std::string_view capture_none_json{
    R"({"truncated":false,"degraded":false,"bytes":0,"omitted_steps":0,"omitted_calls":0,"omitted_logs":0,"omitted_body_bytes":0,"redactions":0})"};

}  // namespace service::attempt_trace
