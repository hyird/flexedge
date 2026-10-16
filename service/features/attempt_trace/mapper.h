#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/attempt_trace/model.h"
#include "service/features/attempt_trace/redaction.h"

namespace service::attempt_trace {

// The serialized trace of one finished attempt, ready to be stored.
struct stored_trace final {
    std::string summary_json_;
    std::string trace_json_;
    std::int64_t trace_bytes_{};
};

[[nodiscard]] inline std::int64_t milliseconds_between(std::chrono::steady_clock::time_point from,
    std::chrono::steady_clock::time_point to) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    return elapsed < 0 ? 0 : elapsed;
}

// What the recorder captured of one attempt, already redacted and bounded.

struct recorded_http final {
    redacted_text method_;
    std::string_view scheme_;
    redacted_text host_;
    redacted_url url_;
    std::vector<redacted_pair> request_headers_;
    std::optional<redacted_body> request_body_;
    int status_{};
    std::string_view transport_error_;
    std::vector<redacted_pair> response_headers_;
    std::optional<redacted_body> response_body_;
};

struct recorded_node_field final {
    redacted_text name_;
    redacted_text value_;
};

struct recorded_node final {
    std::string_view direction_;
    redacted_text message_;
    redacted_text request_id_;
    std::int64_t object_count_{};
    std::int64_t bytes_{};
    std::vector<recorded_node_field> fields_;
};

struct recorded_call final {
    std::string_view kind_;
    std::string_view operation_;
    redacted_text subject_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point last_started_;
    std::int64_t duration_ms_{};
    std::optional<std::int64_t> duration_ms_override_;
    std::string_view outcome_;
    std::int64_t repeat_count_{1};
    std::int64_t repeat_total_ms_{};
    redacted_text provider_request_id_;
    redacted_text provider_code_;
    redacted_text error_;
    std::optional<recorded_http> http_;
    std::optional<dns_observation> dns_;
    std::optional<recorded_node> node_;
    std::size_t bytes_{};
};

struct recorded_log final {
    std::chrono::steady_clock::time_point at_;
    log_level level_{log_level::info};
    redacted_text message_;
    std::int64_t repeat_count_{1};
};

struct recorded_related final {
    redacted_text resource_type_;
    redacted_text resource_id_;
    redacted_text label_;
};

struct recorded_step final {
    step_code code_{step_code::unscoped};
    redacted_text subject_;
    std::chrono::steady_clock::time_point started_;
    std::optional<std::chrono::system_clock::time_point> reported_started_at_;
    // The reporter's phase name of a reported step; never serialized.
    std::string reported_phase_;
    std::int64_t offset_ms_{};
    std::int64_t duration_ms_{};
    step_outcome outcome_{step_outcome::interrupted};
    bool open_{};
    redacted_text error_;
    std::optional<recorded_related> related_;
    std::vector<recorded_call> calls_;
    std::vector<recorded_log> logs_;
    std::int64_t call_count_{};
    std::int64_t omitted_calls_{};
};

// A finished attempt as the recorder hands it to serialization.
struct recorded_trace final {
    trace_kind kind_{};
    std::string_view worker_type_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::system_clock::time_point system_started_;
    std::span<const recorded_step> steps_;
    std::size_t call_total_{};
    std::size_t log_total_{};
    std::size_t omitted_steps_{};
    std::size_t omitted_calls_{};
    std::size_t omitted_logs_{};
    std::size_t omitted_body_bytes_{};
    std::size_t redactions_{};
    bool truncated_{};
    bool degraded_{};
};

// Successive reductions that bring a document under trace_max_bytes.
enum class trace_reduction : std::uint8_t {
    full,
    without_bodies,
    without_calls,
};

[[nodiscard]] inline std::string wall_time_text(std::chrono::system_clock::time_point wall) {
    const auto seconds = std::chrono::floor<std::chrono::seconds>(wall);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(wall - seconds).count();
    const auto time = std::chrono::system_clock::to_time_t(seconds);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &time);
#else
    gmtime_r(&time, &parts);
#endif
    std::array<char, 40> buffer{};
    const auto size = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &parts);
    std::array<char, 16> fraction{};
    const auto fraction_size = std::snprintf(fraction.data(), fraction.size(), ".%06lld+00",
        static_cast<long long>(micros));
    return std::string(buffer.data(), size) +
           std::string(fraction.data(), static_cast<std::size_t>(std::max(fraction_size, 0)));
}

[[nodiscard]] inline std::string time_text(const recorded_trace& trace, std::chrono::steady_clock::time_point at) {
    const auto wall = trace.system_started_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(at - trace.started_);
    return wall_time_text(wall);
}

[[nodiscard]] inline std::string step_started_text(const recorded_trace& trace, const recorded_step& step) {
    return step.reported_started_at_ ? wall_time_text(*step.reported_started_at_) : time_text(trace, step.started_);
}

[[nodiscard]] inline std::string summary_json(const recorded_trace& trace) {
    std::pmr::monotonic_buffer_resource pool;
    struct merged final {
        step_code code_;
        step_outcome outcome_;
        std::int64_t duration_ms_;
        std::int64_t call_count_;
        std::int64_t count_;
    };
    std::vector<merged> entries;
    for (const auto& step : trace.steps_) {
        if (!entries.empty() && entries.back().code_ == step.code_) {
            auto& entry = entries.back();
            if (step_outcome_severity(step.outcome_) > step_outcome_severity(entry.outcome_)) {
                entry.outcome_ = step.outcome_;
            }
            entry.duration_ms_ += step.duration_ms_;
            entry.call_count_ += step.call_count_;
            ++entry.count_;
        } else {
            entries.push_back({step.code_, step.outcome_, step.duration_ms_, step.call_count_, 1});
        }
    }
    const auto failed = std::find_if(trace.steps_.begin(), trace.steps_.end(),
        [](const recorded_step& step) { return step.outcome_ == step_outcome::failed; });
    bool truncated = trace.truncated_ || entries.size() > summary_steps;
    auto shown = std::min(entries.size(), summary_steps);
    while (true) {
        trace_summary_dto summary({.resource_ = &pool});
        summary.set<"state">("available");
        summary.set<"step_count">(static_cast<std::int64_t>(trace.steps_.size() + trace.omitted_steps_));
        summary.set<"call_count">(static_cast<std::int64_t>(trace.call_total_));
        summary.set<"log_count">(static_cast<std::int64_t>(trace.log_total_));
        summary.set<"truncated">(truncated);
        if (failed == trace.steps_.end()) {
            summary.set<"failed_step">(nullptr);
        } else {
            trace_failed_step_dto failed_step({.resource_ = &pool});
            failed_step.set<"code">(describe_step(failed->code_).name_);
            failed_step.set<"label">(describe_step(failed->code_).label_);
            summary.set<"failed_step">(std::move(failed_step));
        }
        auto& steps = summary.ensure<"steps">();
        for (std::size_t index = 0; index < shown; ++index) {
            const auto& entry = entries[index];
            auto& item = steps.emplace_back(ruvia::model_options{.resource_ = &pool});
            item.set<"code">(describe_step(entry.code_).name_);
            item.set<"label">(describe_step(entry.code_).label_);
            item.set<"outcome">(step_outcome_name(entry.outcome_));
            item.set<"duration_ms">(entry.duration_ms_);
            item.set<"call_count">(entry.call_count_);
            item.set<"count">(entry.count_);
        }
        auto json = ruvia::to_json(summary, {.resource_ = &pool});
        if (json.size() <= summary_max_bytes || shown == 0) {
            return std::string(json);
        }
        --shown;
        truncated = true;
    }
}

inline void fill_pairs(ruvia::array<trace_pair_dto>& output, const std::vector<redacted_pair>& pairs,
    std::pmr::memory_resource* pool) {
    for (const auto& pair : pairs) {
        auto& item = output.emplace_back(ruvia::model_options{.resource_ = pool});
        item.set<"name">(pair.name_.view());
        item.set<"value">(pair.value_.view());
        item.set<"redacted">(pair.redacted_);
    }
}

template <typename field_type>
void fill_body(field_type&& assign, const std::optional<redacted_body>& body, bool keep,
    std::pmr::memory_resource* pool) {
    if (!body) {
        assign(nullptr);
        return;
    }
    trace_body_dto output({.resource_ = pool});
    output.set<"content_type">(body->content_type_.view());
    output.set<"bytes">(body->bytes_);
    if (keep || !body->omitted_reason_.empty()) {
        output.set<"format">(body->format_);
        output.set<"excerpt">(body->excerpt_.view());
        output.set<"truncated">(body->truncated_);
        output.set<"omitted_reason">(body->omitted_reason_);
    } else {
        output.set<"format">("omitted");
        output.set<"excerpt">("");
        output.set<"truncated">(false);
        output.set<"omitted_reason">("budget");
    }
    assign(std::move(output));
}

[[nodiscard]] inline std::string document_json(const recorded_trace& trace, trace_reduction reduction) {
    std::pmr::monotonic_buffer_resource pool;
    const bool keep_bodies = reduction == trace_reduction::full;
    const bool keep_calls = reduction != trace_reduction::without_calls;
    std::size_t omitted_calls = trace.omitted_calls_;
    std::size_t omitted_logs = trace.omitted_logs_;
    std::size_t omitted_body_bytes = trace.omitted_body_bytes_;
    trace_document_dto document({.resource_ = &pool});
    document.set<"kind">(trace_kind_name(trace.kind_));
    document.set<"worker_type">(trace.worker_type_);
    auto& steps = document.ensure<"steps">();
    std::int64_t index = 0;
    for (const auto& step : trace.steps_) {
        auto& item = steps.emplace_back(ruvia::model_options{.resource_ = &pool});
        const auto& descriptor = describe_step(step.code_);
        item.set<"index">(index++);
        item.set<"code">(descriptor.name_);
        item.set<"label">(descriptor.label_);
        item.set<"subject">(step.subject_.view());
        item.set<"started_at">(step_started_text(trace, step));
        item.set<"offset_ms">(step.offset_ms_);
        item.set<"duration_ms">(step.duration_ms_);
        item.set<"outcome">(step_outcome_name(step.open_ ? step_outcome::interrupted : step.outcome_));
        item.set<"error">(step.error_.view());
        if (step.related_) {
            trace_related_task_dto related({.resource_ = &pool});
            related.set<"resource_type">(step.related_->resource_type_.view());
            related.set<"resource_id">(step.related_->resource_id_.view());
            related.set<"label">(step.related_->label_.view());
            item.set<"related_task">(std::move(related));
        } else {
            item.set<"related_task">(nullptr);
        }
        item.set<"call_count">(step.call_count_);
        auto& calls = item.ensure<"calls">();
        std::int64_t call_index = 0;
        for (const auto& call : step.calls_) {
            if (!keep_calls) {
                ++omitted_calls;
                continue;
            }
            auto& output = calls.emplace_back(ruvia::model_options{.resource_ = &pool});
            output.set<"index">(call_index++);
            output.set<"kind">(call.kind_);
            output.set<"operation">(call.operation_);
            output.set<"subject">(call.subject_.view());
            output.set<"started_at">(time_text(trace, call.started_));
            output.set<"offset_ms">(milliseconds_between(trace.started_, call.started_));
            output.set<"duration_ms">(call.duration_ms_);
            output.set<"outcome">(call.outcome_);
            output.set<"repeat_count">(call.repeat_count_);
            output.set<"repeat_total_ms">(call.repeat_total_ms_);
            output.set<"last_started_at">(time_text(trace, call.last_started_));
            output.set<"provider_request_id">(call.provider_request_id_.view());
            output.set<"provider_code">(call.provider_code_.view());
            output.set<"error">(call.error_.view());
            if (call.http_) {
                const auto& http = *call.http_;
                trace_http_dto value({.resource_ = &pool});
                value.set<"method">(http.method_.view());
                value.set<"url">(http.url_.url_.view());
                value.set<"scheme">(http.scheme_);
                value.set<"host">(http.host_.view());
                value.set<"path">(http.url_.path_.view());
                fill_pairs(value.ensure<"query">(), http.url_.query_, &pool);
                fill_pairs(value.ensure<"request_headers">(), http.request_headers_, &pool);
                fill_body([&](auto&& body) { value.set<"request_body">(std::forward<decltype(body)>(body)); },
                    http.request_body_, keep_bodies, &pool);
                value.set<"status">(static_cast<std::int64_t>(http.status_));
                value.set<"transport_error">(http.transport_error_);
                fill_pairs(value.ensure<"response_headers">(), http.response_headers_, &pool);
                fill_body([&](auto&& body) { value.set<"response_body">(std::forward<decltype(body)>(body)); },
                    http.response_body_, keep_bodies, &pool);
                if (!keep_bodies) {
                    for (const auto* body : {&http.request_body_, &http.response_body_}) {
                        if (*body && (*body)->omitted_reason_.empty()) {
                            omitted_body_bytes += static_cast<std::size_t>((*body)->bytes_);
                        }
                    }
                }
                output.set<"http">(std::move(value));
            } else {
                output.set<"http">(nullptr);
            }
            if (call.dns_) {
                const auto& dns = *call.dns_;
                trace_dns_dto value({.resource_ = &pool});
                value.set<"resolver">(call.http_ ? call.http_->host_.view() : std::string_view{});
                value.set<"transport">("doh_json");
                value.set<"name">(dns.name_.view());
                value.set<"type">(dns.type_.view());
                value.set<"rcode">(dns.rcode_);
                auto& answers = value.ensure<"answers">();
                for (const auto& answer : dns.answers_) {
                    auto& entry = answers.emplace_back(ruvia::model_options{.resource_ = &pool});
                    entry.set<"name">(answer.name_.view());
                    entry.set<"type">(answer.type_.view());
                    entry.set<"ttl">(answer.ttl_);
                    entry.set<"data">(answer.data_.view());
                }
                value.set<"truncated">(dns.truncated_);
                output.set<"dns">(std::move(value));
            } else {
                output.set<"dns">(nullptr);
            }
            if (call.node_) {
                const auto& node = *call.node_;
                trace_node_message_dto value({.resource_ = &pool});
                value.set<"direction">(node.direction_);
                value.set<"message">(node.message_.view());
                value.set<"request_id">(node.request_id_.view());
                value.set<"object_count">(node.object_count_);
                value.set<"bytes">(node.bytes_);
                auto& fields = value.ensure<"fields">();
                for (const auto& field : node.fields_) {
                    auto& entry = fields.emplace_back(ruvia::model_options{.resource_ = &pool});
                    entry.set<"name">(field.name_.view());
                    entry.set<"value">(field.value_.view());
                }
                output.set<"node_message">(std::move(value));
            } else {
                output.set<"node_message">(nullptr);
            }
        }
        item.set<"omitted_calls">(step.omitted_calls_ + (keep_calls ? 0 : static_cast<std::int64_t>(step.calls_.size())));
        auto& logs = item.ensure<"logs">();
        for (const auto& log : step.logs_) {
            if (!keep_bodies) {
                ++omitted_logs;
                continue;
            }
            auto& output = logs.emplace_back(ruvia::model_options{.resource_ = &pool});
            output.set<"at">(time_text(trace, log.at_));
            output.set<"offset_ms">(milliseconds_between(trace.started_, log.at_));
            output.set<"level">(log_level_name(log.level_));
            output.set<"message">(log.message_.view());
            output.set<"repeat_count">(log.repeat_count_);
        }
    }
    // The stored size is reported in the document itself; repeated passes
    // make the number include its own digits.
    std::int64_t bytes = 0;
    std::string json;
    for (int pass = 0; pass < 3; ++pass) {
        trace_capture_dto capture({.resource_ = &pool});
        capture.set<"truncated">(trace.truncated_ || reduction != trace_reduction::full);
        capture.set<"degraded">(trace.degraded_);
        capture.set<"bytes">(bytes);
        capture.set<"omitted_steps">(static_cast<std::int64_t>(trace.omitted_steps_));
        capture.set<"omitted_calls">(static_cast<std::int64_t>(omitted_calls));
        capture.set<"omitted_logs">(static_cast<std::int64_t>(omitted_logs));
        capture.set<"omitted_body_bytes">(static_cast<std::int64_t>(omitted_body_bytes));
        capture.set<"redactions">(static_cast<std::int64_t>(trace.redactions_));
        document.set<"capture">(std::move(capture));
        json = std::string(ruvia::to_json(document, {.resource_ = &pool}));
        if (static_cast<std::int64_t>(json.size()) == bytes) {
            break;
        }
        bytes = static_cast<std::int64_t>(json.size());
    }
    return json;
}

// Maps a finished attempt to its stored summary and document, reduced until
// the document fits trace_max_bytes; nothing when even the smallest does not.
[[nodiscard]] inline std::optional<stored_trace> to_stored_trace(const recorded_trace& trace) {
    stored_trace result;
    result.summary_json_ = summary_json(trace);
    for (const auto reduction : {trace_reduction::full, trace_reduction::without_bodies,
             trace_reduction::without_calls}) {
        auto json = document_json(trace, reduction);
        if (json.size() <= trace_max_bytes) {
            result.trace_bytes_ = static_cast<std::int64_t>(json.size());
            result.trace_json_ = std::move(json);
            return result;
        }
    }
    return std::nullopt;
}

}  // namespace service::attempt_trace
