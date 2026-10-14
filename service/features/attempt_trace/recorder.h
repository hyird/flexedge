#pragma once

#include <ruvia/http/http_client.h>
#include <ruvia/web/http_client_types.h>
#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/config/outbound.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/attempt_trace/redaction.h"

namespace service::attempt_trace {

// The serialized trace of one finished attempt, ready to be stored.
struct stored_trace final {
    std::string summary_json_;
    std::string trace_json_;
    std::int64_t trace_bytes_{};
};

// Another task this step waited for (a certificate waits for DNS sync).
struct related_task final {
    std::string_view task_id_;
    std::string_view resource_type_;
    std::string_view resource_id_;
    std::string_view label_;
};

// One message on a node control connection, already reduced to the fields
// that may be shown (never object payloads, digests or revisions).
struct node_message_field final {
    std::string_view name_;
    std::string_view value_;
};

struct node_message final {
    std::string_view direction_;
    std::string_view message_;
    std::string_view request_id_;
    std::int64_t object_count_{};
    std::int64_t bytes_{};
    std::span<const node_message_field> fields_;
};

class recorder;

// Boundary of one step. A scope left without complete(), skip() or fail()
// (an exception unwinding through it) records the step as failed.
class step_scope final {
public:
    step_scope() noexcept = default;

    step_scope(step_scope&& other) noexcept
        : recorder_(std::exchange(other.recorder_, nullptr)),
          index_(other.index_) {}

    step_scope& operator=(step_scope&&) = delete;
    step_scope(const step_scope&) = delete;
    step_scope& operator=(const step_scope&) = delete;

    ~step_scope();

    void complete() noexcept;
    void skip(std::string_view reason = {}) noexcept;
    void fail(std::string_view error) noexcept;

private:
    friend class recorder;

    step_scope(recorder* owner, std::size_t index) noexcept
        : recorder_(owner),
          index_(index) {}

    recorder* recorder_{};
    std::size_t index_{};
};

// Identifies the outbound call in flight; calls of one attempt are sequential.
struct call_token final {
    std::uint64_t id_{};
};

// Attempt-scoped trace recorder, owned by the worker coroutine that runs the
// attempt and passed by reference to everything the attempt calls. Every
// operation is noexcept: an internal failure stops capturing (the trace is
// marked degraded) and never changes the attempt itself.
class recorder final {
public:
    recorder(trace_kind kind, std::string_view worker_type, std::chrono::steady_clock::time_point started) noexcept
        : kind_(kind),
          worker_type_(worker_type),
          started_(started == std::chrono::steady_clock::time_point{} ? std::chrono::steady_clock::now() : started) {
        const auto now = std::chrono::steady_clock::now();
        system_started_ = std::chrono::system_clock::now() -
                          std::chrono::duration_cast<std::chrono::system_clock::duration>(now - started_);
    }

    recorder(const recorder&) = delete;
    recorder& operator=(const recorder&) = delete;

    [[nodiscard]] step_scope begin_step(step_code code, std::string_view subject = {}) noexcept {
        if (finished_ || degraded_) {
            return {};
        }
        try {
            if (steps_.size() >= max_steps) {
                ++omitted_steps_;
                truncated_ = true;
                open_.push_back(dropped_step);
                return step_scope(this, dropped_step);
            }
            auto& step = steps_.emplace_back();
            step.code_ = code;
            step.subject_ = redactor_.text(subject, value_bytes);
            step.started_ = std::chrono::steady_clock::now();
            step.offset_ms_ = milliseconds_since(started_, step.started_);
            step.open_ = true;
            bytes_ += step_overhead + text_detail::json_string_size(step.subject_.view());
            open_.push_back(steps_.size() - 1);
            return step_scope(this, steps_.size() - 1);
        } catch (...) {
            degraded_ = true;
            return {};
        }
    }

    void log(log_level level, std::string_view message) noexcept {
        if (finished_ || degraded_) {
            return;
        }
        try {
            ++log_total_;
            auto* step = current_step();
            if (step == nullptr) {
                ++omitted_logs_;
                truncated_ = true;
                return;
            }
            append_log(*step, level, message, std::chrono::steady_clock::now());
        } catch (...) {
            degraded_ = true;
        }
    }

    // A credential of this attempt; see redactor::register_secret.
    void register_secret(std::string_view value) noexcept {
        redactor_.register_secret(value);
    }

    // Error text leaving the attempt (task error, sys_sync_event.error, a
    // resource's last error, the server log): this attempt's credentials and
    // the credential patterns are replaced, whatever the capture state.
    [[nodiscard]] std::string redact_error(std::string_view error) noexcept {
        return redactor_.external_text(error, redacted_error_fallback);
    }

    // Attaches the attempt error to the step that failed (the first failed
    // step without an error), or records it on its own when none failed.
    void mark_failure(std::string_view error) noexcept {
        if (finished_ || degraded_) {
            return;
        }
        try {
            for (auto& step : steps_) {
                if (step.outcome_ == step_outcome::failed && step.error_.empty() && !step.open_) {
                    step.error_ = redactor_.text(error, error_bytes);
                    bytes_ += text_detail::json_string_size(step.error_.view());
                    return;
                }
            }
            if (!open_.empty() && open_.back() != dropped_step) {
                auto& step = steps_[open_.back()];
                step.error_ = redactor_.text(error, error_bytes);
                bytes_ += text_detail::json_string_size(step.error_.view());
                return;
            }
            if (std::any_of(steps_.begin(), steps_.end(), [](const auto& step) {
                    return step.outcome_ == step_outcome::failed && !step.error_.empty();
                })) {
                return;
            }
            if (steps_.size() >= max_steps) {
                truncated_ = true;
                return;
            }
            auto& step = steps_.emplace_back();
            step.code_ = step_code::unscoped;
            step.started_ = std::chrono::steady_clock::now();
            step.offset_ms_ = milliseconds_since(started_, step.started_);
            step.outcome_ = step_outcome::failed;
            step.error_ = redactor_.text(error, error_bytes);
            bytes_ += step_overhead + text_detail::json_string_size(step.error_.view());
        } catch (...) {
            degraded_ = true;
        }
    }

    void link_task(const related_task& task) noexcept {
        if (finished_ || degraded_ || open_.empty() || open_.back() == dropped_step) {
            return;
        }
        try {
            auto& step = steps_[open_.back()];
            step.related_ = related_record{.task_id_ = redactor_.text(task.task_id_, identifier_bytes),
                .resource_type_ = redactor_.text(task.resource_type_, identifier_bytes),
                .resource_id_ = redactor_.text(task.resource_id_, identifier_bytes),
                .label_ = redactor_.text(task.label_, value_bytes)};
            bytes_ += related_overhead;
        } catch (...) {
            degraded_ = true;
        }
    }

    // Outbound HTTP capture; used by service::outbound_http::send_buffered.
    [[nodiscard]] call_token begin_http(const outbound_call& call, const ruvia::http_client_request_view& request) noexcept {
        if (finished_ || degraded_) {
            return {};
        }
        try {
            abandon_pending();
            const auto& policy = policy_for(call.operation_);
            const auto& origin =
                service::config::outbound_origin_config(service::config::get_outbound_origins(), call.origin_alias_);
            const auto scheme = origin.scheme_ == ruvia::http_scheme::https ? std::string_view{"https"} : std::string_view{"http"};
            pending_call pending;
            pending.id_ = ++next_call_id_;
            pending.policy_ = &policy;
            auto& record = pending.record_;
            record.kind_ = policy.dns_query_ ? "dns" : "http";
            record.operation_ = policy.code_;
            record.subject_ = redactor_.text(call.subject_, value_bytes);
            record.started_ = std::chrono::steady_clock::now();
            auto& http = record.http_.emplace();
            http.method_ = redactor_.text(request.method_.view(), identifier_bytes);
            http.scheme_ = scheme;
            http.host_ = redactor_.text(origin.host_, value_bytes);
            std::string_view content_type;
            std::size_t headers = 0;
            for (const auto& header : request.headers_) {
                if (text_detail::equals_ignore_case(header.name(), "content-type")) {
                    content_type = header.value();
                }
                auto pair = redactor_.request_header(header.name(), header.value());
                if (headers++ < max_headers) {
                    http.request_headers_.push_back(std::move(pair));
                }
            }
            const auto target = request.target_.view();
            http.url_ = redactor_.url(policy, scheme, origin.host_, target);
            if (policy.dns_query_) {
                for (const auto& pair : http.url_.query_) {
                    if (pair.name_.view() == "name") {
                        pending.query_name_ = std::string(pair.value_.view());
                    } else if (pair.name_.view() == "type") {
                        pending.query_type_ = std::string(pair.value_.view());
                    }
                }
            }
            if (const auto* content = request.content_.borrowed_bytes(); content != nullptr && !content->value().empty()) {
                http.request_body_ = capture_body(policy, true, content_type, content->value(), request_excerpt_bytes);
            }
            pending_ = std::move(pending);
            return {.id_ = pending_->id_};
        } catch (...) {
            degraded_ = true;
            pending_.reset();
            return {};
        }
    }

    void complete_http(call_token token, int status, std::span<const header_entry> headers,
        std::string_view body) noexcept {
        if (!pending_ || pending_->id_ != token.id_) {
            return;
        }
        try {
            auto pending = std::move(*pending_);
            pending_.reset();
            auto& record = pending.record_;
            auto& http = *record.http_;
            http.status_ = status;
            std::string_view content_type;
            std::size_t count = 0;
            for (const auto& header : headers) {
                if (text_detail::equals_ignore_case(header.name_, "content-type")) {
                    content_type = header.value_;
                }
                auto pair = redactor_.response_header(header.name_, header.value_);
                if (count++ < max_headers) {
                    http.response_headers_.push_back(std::move(pair));
                }
            }
            if (!body.empty()) {
                http.response_body_ = capture_body(*pending.policy_, false, content_type, body, response_excerpt_bytes);
            }
            auto facts = redactor_.response(*pending.policy_, headers, content_type, body, status, pending.query_name_,
                pending.query_type_);
            record.provider_request_id_ = std::move(facts.request_id_);
            record.provider_code_ = std::move(facts.provider_code_);
            record.dns_ = std::move(facts.dns_);
            if (pending.policy_->dns_query_ && !record.dns_) {
                record.dns_ = redactor_.unanswered_query(pending.query_name_, pending.query_type_);
            }
            record.outcome_ = status >= 200 && status < 300 ? "ok" : "http_error";
            store_call(std::move(record));
        } catch (...) {
            degraded_ = true;
        }
    }

    void fail_http(call_token token, ruvia::http_client_error::code_type code) noexcept {
        if (!pending_ || pending_->id_ != token.id_) {
            return;
        }
        try {
            auto pending = std::move(*pending_);
            pending_.reset();
            auto& record = pending.record_;
            const auto name = transport_error_name(code);
            record.http_->transport_error_ = name;
            record.outcome_ = name == "cancelled" ? "cancelled" : "transport_error";
            record.error_ = redactor_.text(name, identifier_bytes);
            if (pending.policy_->dns_query_) {
                record.dns_ = redactor_.unanswered_query(pending.query_name_, pending.query_type_);
            }
            store_call(std::move(record));
        } catch (...) {
            degraded_ = true;
        }
    }

    // The caller could not use a successful response of the last call.
    void mark_invalid_response(std::string_view reason) noexcept {
        if (finished_ || degraded_ || !last_call_) {
            return;
        }
        try {
            auto& call = steps_[last_call_->first].calls_[last_call_->second];
            if (call.outcome_ == "ok") {
                call.outcome_ = "invalid_response";
                call.error_ = redactor_.text(reason, error_bytes);
                bytes_ += text_detail::json_string_size(call.error_.view());
            }
        } catch (...) {
            degraded_ = true;
        }
    }

    // A control-connection message of a node release attempt.
    void record_node_message(const node_message& message, std::chrono::steady_clock::time_point at) noexcept {
        if (finished_ || degraded_) {
            return;
        }
        try {
            call_record record;
            record.kind_ = "node_message";
            record.operation_ = message.direction_ == "sent" ? "node.send" : "node.receive";
            record.started_ = at == std::chrono::steady_clock::time_point{} ? std::chrono::steady_clock::now() : at;
            record.outcome_ = "ok";
            auto& node = record.node_.emplace();
            node.direction_ = message.direction_ == "sent" ? "sent" : "received";
            node.message_ = redactor_.text(message.message_, identifier_bytes);
            node.request_id_ = redactor_.text(message.request_id_, identifier_bytes);
            node.object_count_ = message.object_count_;
            node.bytes_ = message.bytes_;
            for (const auto& field : message.fields_) {
                node.fields_.push_back({.name_ = redactor_.text(field.name_, identifier_bytes),
                    .value_ = redactor_.text(field.value_, error_bytes)});
            }
            record.duration_ms_override_ = 0;
            store_call(std::move(record));
        } catch (...) {
            degraded_ = true;
        }
    }

    // A step reported by another process (a node's apply phase), with its own
    // wall-clock start and monotonic duration. Clock skew never yields a
    // negative offset. `phase` is the reporter's name of the step, which its
    // log lines refer to.
    void record_reported_step(step_code code, std::string_view phase, std::string_view subject,
        std::chrono::system_clock::time_point started_at, std::int64_t duration_ms, bool ok,
        std::string_view error) noexcept {
        if (finished_ || degraded_) {
            return;
        }
        try {
            if (steps_.size() >= max_steps) {
                ++omitted_steps_;
                truncated_ = true;
                return;
            }
            auto& step = steps_.emplace_back();
            step.code_ = code;
            step.subject_ = redactor_.text(subject, value_bytes);
            step.reported_started_at_ = started_at;
            // Only compared with log phases; a name longer than any real
            // phase is not kept and never matches.
            if (phase.size() <= max_reported_phase_bytes) {
                step.reported_phase_ = std::string(phase);
            }
            step.started_ = started_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                           started_at - system_started_);
            step.offset_ms_ = std::max<std::int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(started_at - system_started_).count(), 0);
            step.duration_ms_ = std::max<std::int64_t>(duration_ms, 0);
            step.outcome_ = ok ? step_outcome::completed : step_outcome::failed;
            step.error_ = ok ? redacted_text{} : redactor_.text(error, error_bytes);
            bytes_ += step_overhead + text_detail::json_string_size(step.subject_.view()) +
                      text_detail::json_string_size(step.error_.view());
        } catch (...) {
            degraded_ = true;
        }
    }

    // A log line reported by another process. A line naming its step's phase
    // belongs to the most recent reported step with that phase, wherever its
    // timestamp falls (a node writes a step's log after the step ends). A line
    // without a phase, or naming a phase no recorded step has, goes to the
    // latest reported step that had started by then (the first reported step,
    // or the last step, when none had).
    void record_reported_log(log_level level, std::chrono::system_clock::time_point at, std::string_view phase,
        std::string_view message) noexcept {
        if (finished_ || degraded_) {
            return;
        }
        try {
            ++log_total_;
            if (steps_.empty()) {
                ++omitted_logs_;
                truncated_ = true;
                return;
            }
            step_record* by_phase = nullptr;
            step_record* by_time = nullptr;
            for (auto& step : steps_) {
                if (!step.reported_started_at_) {
                    continue;
                }
                if (!phase.empty() && step.reported_phase_ == phase) {
                    by_phase = &step;
                }
                if (by_time == nullptr || *step.reported_started_at_ <= at) {
                    by_time = &step;
                }
            }
            auto* owner = by_phase != nullptr ? by_phase : by_time;
            append_log(owner != nullptr ? *owner : steps_.back(), level, message,
                started_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(at - system_started_));
        } catch (...) {
            degraded_ = true;
        }
    }

    // Closes the attempt: steps still open when the result committed are
    // completed, an unfinished call is cancelled. Returns nothing only when
    // even a reduced trace cannot be serialized within the cap.
    [[nodiscard]] std::optional<stored_trace> finish() noexcept {
        if (finished_) {
            return std::nullopt;
        }
        finished_ = true;
        try {
            abandon_pending();
            const auto now = std::chrono::steady_clock::now();
            for (const auto index : open_) {
                if (index != dropped_step) {
                    close_step(steps_[index], step_outcome::completed, now);
                }
            }
            open_.clear();
            // Steps reported by a node interleave with the steps recorded here.
            std::stable_sort(steps_.begin(), steps_.end(),
                [](const step_record& left, const step_record& right) { return left.offset_ms_ < right.offset_ms_; });
            last_call_.reset();
            stored_trace result;
            result.summary_json_ = summary_json();
            for (const auto reduction : {reduction_level::full, reduction_level::without_bodies,
                     reduction_level::without_calls}) {
                auto json = document_json(reduction);
                if (json.size() <= trace_max_bytes) {
                    result.trace_bytes_ = static_cast<std::int64_t>(json.size());
                    result.trace_json_ = std::move(json);
                    return result;
                }
                truncated_ = true;
            }
        } catch (...) {
            degraded_ = true;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool degraded() const noexcept {
        return degraded_ || redactor_.failed();
    }

    [[nodiscard]] trace_kind kind() const noexcept {
        return kind_;
    }

private:
    friend class step_scope;

    static constexpr std::size_t dropped_step = static_cast<std::size_t>(-1);
    static constexpr std::size_t max_reported_phase_bytes{64};
    static constexpr std::size_t step_overhead{420};
    static constexpr std::size_t call_overhead{720};
    static constexpr std::size_t pair_overhead{48};
    static constexpr std::size_t body_overhead{140};
    static constexpr std::size_t dns_overhead{160};
    static constexpr std::size_t answer_overhead{64};
    static constexpr std::size_t log_overhead{120};
    static constexpr std::size_t related_overhead{360};
    // Room kept for the document envelope and the closing of open steps.
    static constexpr std::size_t reserved_bytes{16384};

    enum class reduction_level : std::uint8_t {
        full,
        without_bodies,
        without_calls,
    };

    struct http_record final {
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

    struct node_field_record final {
        redacted_text name_;
        redacted_text value_;
    };

    struct node_record final {
        std::string_view direction_;
        redacted_text message_;
        redacted_text request_id_;
        std::int64_t object_count_{};
        std::int64_t bytes_{};
        std::vector<node_field_record> fields_;
    };

    struct call_record final {
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
        std::optional<http_record> http_;
        std::optional<dns_observation> dns_;
        std::optional<node_record> node_;
        std::size_t bytes_{};
    };

    struct log_record final {
        std::chrono::steady_clock::time_point at_;
        log_level level_{log_level::info};
        redacted_text message_;
        std::int64_t repeat_count_{1};
    };

    struct related_record final {
        redacted_text task_id_;
        redacted_text resource_type_;
        redacted_text resource_id_;
        redacted_text label_;
    };

    struct step_record final {
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
        std::optional<related_record> related_;
        std::vector<call_record> calls_;
        std::vector<log_record> logs_;
        std::int64_t call_count_{};
        std::int64_t omitted_calls_{};
    };

    struct pending_call final {
        std::uint64_t id_{};
        const outbound_policy* policy_{};
        call_record record_;
        std::string query_name_;
        std::string query_type_;
    };

    [[nodiscard]] static std::int64_t milliseconds_since(std::chrono::steady_clock::time_point from,
        std::chrono::steady_clock::time_point to) noexcept {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
        return elapsed < 0 ? 0 : elapsed;
    }

    [[nodiscard]] static std::string_view transport_error_name(ruvia::http_client_error::code_type code) noexcept {
        using code_type = ruvia::http_client_error::code_type;
        switch (code) {
            case code_type::timeout:
                return "timeout";
            case code_type::resolve_failed:
                return "resolve_failed";
            case code_type::connect_failed:
                return "connect_failed";
            case code_type::tls_failed:
                return "tls_failed";
            case code_type::response_too_large:
                return "response_too_large";
            case code_type::io_error:
                return "io_error";
            case code_type::protocol_unavailable:
            case code_type::protocol_error:
                return "protocol_error";
            case code_type::queue_full:
                return "queue_full";
            case code_type::cancelled:
            case code_type::closing:
                return "cancelled";
            default:
                return "other";
        }
    }

    [[nodiscard]] bool fits(std::size_t size) const noexcept {
        return bytes_ + size + reserved_bytes <= trace_max_bytes;
    }

    // Adjacent identical lines of one step fold into a repeat count.
    void append_log(step_record& step, log_level level, std::string_view message,
        std::chrono::steady_clock::time_point at) {
        auto text = redactor_.text(message, log_message_bytes);
        if (!step.logs_.empty() && step.logs_.back().level_ == level && step.logs_.back().message_ == text) {
            ++step.logs_.back().repeat_count_;
            return;
        }
        const auto size = log_overhead + text_detail::json_string_size(text.view());
        if (log_records_ >= max_logs || log_bytes_ + size > max_log_bytes || !fits(size)) {
            ++omitted_logs_;
            truncated_ = true;
            return;
        }
        step.logs_.push_back({.at_ = at, .level_ = level, .message_ = std::move(text), .repeat_count_ = 1});
        ++log_records_;
        log_bytes_ += size;
        bytes_ += size;
    }

    // Innermost open step; calls and logs outside any step go to an
    // "unscoped" step so nothing captured is silently lost.
    step_record* current_step() {
        if (!open_.empty()) {
            return open_.back() == dropped_step ? nullptr : &steps_[open_.back()];
        }
        if (!steps_.empty() && steps_.back().code_ == step_code::unscoped && !steps_.back().reported_started_at_) {
            return &steps_.back();
        }
        if (steps_.size() >= max_steps) {
            return nullptr;
        }
        auto& step = steps_.emplace_back();
        step.code_ = step_code::unscoped;
        step.started_ = std::chrono::steady_clock::now();
        step.offset_ms_ = milliseconds_since(started_, step.started_);
        step.outcome_ = step_outcome::completed;
        bytes_ += step_overhead;
        return &step;
    }

    [[nodiscard]] redacted_body capture_body(const outbound_policy& policy, bool request, std::string_view content_type,
        std::string_view body, std::size_t limit) {
        const auto budget = body_used_ < body_budget_bytes ? body_budget_bytes - body_used_ : 0;
        auto result = redactor_.body(policy, request, content_type, body, limit, budget);
        if (result.omitted_reason_.empty()) {
            body_used_ += text_detail::json_string_size(result.excerpt_.view());
            if (result.truncated_) {
                omitted_body_bytes_ += body.size() > result.excerpt_.view().size() ? body.size() - result.excerpt_.view().size() : 0;
                truncated_ = true;
            }
        } else if (result.omitted_reason_ == "budget" || result.omitted_reason_ == "too_large") {
            omitted_body_bytes_ += body.size();
            truncated_ = true;
        }
        return result;
    }

    [[nodiscard]] static std::size_t pairs_size(const std::vector<redacted_pair>& pairs) noexcept {
        std::size_t size = 0;
        for (const auto& pair : pairs) {
            size += pair_overhead + text_detail::json_string_size(pair.name_.view()) +
                    text_detail::json_string_size(pair.value_.view());
        }
        return size;
    }

    [[nodiscard]] static std::size_t body_size(const std::optional<redacted_body>& body) noexcept {
        return body ? body_overhead + text_detail::json_string_size(body->excerpt_.view()) +
                          text_detail::json_string_size(body->content_type_.view())
                    : 0;
    }

    [[nodiscard]] static std::size_t call_size(const call_record& record) noexcept {
        auto size = call_overhead + text_detail::json_string_size(record.subject_.view()) +
                    text_detail::json_string_size(record.provider_request_id_.view()) +
                    text_detail::json_string_size(record.provider_code_.view()) +
                    text_detail::json_string_size(record.error_.view());
        if (record.http_) {
            const auto& http = *record.http_;
            size += text_detail::json_string_size(http.url_.url_.view()) * 2 + pairs_size(http.url_.query_) +
                    pairs_size(http.request_headers_) + pairs_size(http.response_headers_) +
                    body_size(http.request_body_) + body_size(http.response_body_) +
                    text_detail::json_string_size(http.host_.view());
        }
        if (record.dns_) {
            size += dns_overhead + text_detail::json_string_size(record.dns_->name_.view());
            for (const auto& answer : record.dns_->answers_) {
                size += answer_overhead + text_detail::json_string_size(answer.name_.view()) +
                        text_detail::json_string_size(answer.data_.view());
            }
        }
        if (record.node_) {
            size += dns_overhead + text_detail::json_string_size(record.node_->request_id_.view());
            for (const auto& field : record.node_->fields_) {
                size += pair_overhead + text_detail::json_string_size(field.name_.view()) +
                        text_detail::json_string_size(field.value_.view());
            }
        }
        return size;
    }

    // Two calls are the same request when everything shown about them
    // except the time matches (ACME polling, nonce fetches).
    [[nodiscard]] static bool same_call(const call_record& left, const call_record& right) noexcept {
        if (left.kind_ != right.kind_ || left.operation_ != right.operation_ || left.outcome_ != right.outcome_ ||
            left.subject_ != right.subject_ || left.provider_code_ != right.provider_code_ ||
            left.error_ != right.error_ || left.http_.has_value() != right.http_.has_value() || left.node_ || right.node_) {
            return false;
        }
        if (!left.http_) {
            return true;
        }
        const auto& a = *left.http_;
        const auto& b = *right.http_;
        const auto same_body = [](const std::optional<redacted_body>& x, const std::optional<redacted_body>& y) {
            return x.has_value() == y.has_value() &&
                   (!x || (x->excerpt_ == y->excerpt_ && x->omitted_reason_ == y->omitted_reason_ && x->bytes_ == y->bytes_));
        };
        return a.method_ == b.method_ && a.url_.url_ == b.url_.url_ && a.status_ == b.status_ &&
               a.transport_error_ == b.transport_error_ && same_body(a.request_body_, b.request_body_) &&
               same_body(a.response_body_, b.response_body_);
    }

    void store_call(call_record record) {
        const auto now = std::chrono::steady_clock::now();
        record.duration_ms_ = record.duration_ms_override_.value_or(milliseconds_since(record.started_, now));
        record.repeat_total_ms_ = record.duration_ms_;
        record.last_started_ = record.started_;
        ++call_total_;
        auto* step = current_step();
        if (step == nullptr) {
            ++omitted_calls_;
            truncated_ = true;
            return;
        }
        ++step->call_count_;
        const auto calls = step->calls_.size();
        for (std::size_t back = 1; back <= 2 && back <= calls; ++back) {
            auto& previous = step->calls_[calls - back];
            if (same_call(previous, record)) {
                ++previous.repeat_count_;
                previous.repeat_total_ms_ += record.duration_ms_;
                previous.last_started_ = record.started_;
                last_call_.emplace(static_cast<std::size_t>(step - steps_.data()), calls - back);
                return;
            }
        }
        record.bytes_ = call_size(record);
        if (stored_calls_ >= max_calls || step->calls_.size() >= max_step_calls) {
            omit_call(*step);
            return;
        }
        if (!fits(record.bytes_) && record.http_) {
            for (auto* body : {&record.http_->request_body_, &record.http_->response_body_}) {
                if (*body && (*body)->omitted_reason_.empty()) {
                    omitted_body_bytes_ += static_cast<std::size_t>((*body)->bytes_);
                    (*body)->excerpt_ = redacted_text{};
                    (*body)->format_ = "omitted";
                    (*body)->omitted_reason_ = "budget";
                    (*body)->truncated_ = false;
                }
            }
            truncated_ = true;
            record.bytes_ = call_size(record);
        }
        if (!fits(record.bytes_)) {
            omit_call(*step);
            return;
        }
        bytes_ += record.bytes_;
        ++stored_calls_;
        step->calls_.push_back(std::move(record));
        last_call_.emplace(static_cast<std::size_t>(step - steps_.data()), step->calls_.size() - 1);
    }

    void omit_call(step_record& step) noexcept {
        ++step.omitted_calls_;
        ++omitted_calls_;
        truncated_ = true;
        last_call_.reset();
    }

    void abandon_pending() {
        if (!pending_) {
            return;
        }
        auto pending = std::move(*pending_);
        pending_.reset();
        pending.record_.outcome_ = "cancelled";
        pending.record_.http_->transport_error_ = "cancelled";
        pending.record_.error_ = redactor_.text("cancelled", identifier_bytes);
        store_call(std::move(pending.record_));
    }

    void close_step(step_record& step, step_outcome outcome, std::chrono::steady_clock::time_point now) noexcept {
        if (!step.open_) {
            return;
        }
        step.open_ = false;
        step.outcome_ = outcome;
        step.duration_ms_ = milliseconds_since(step.started_, now);
    }

    void end_step(std::size_t index, step_outcome outcome, std::string_view message) noexcept {
        if (finished_) {
            return;
        }
        try {
            const auto found = std::find(open_.rbegin(), open_.rend(), index);
            if (found == open_.rend()) {
                return;
            }
            open_.erase(std::next(found).base());
            if (index == dropped_step || degraded_) {
                return;
            }
            auto& step = steps_[index];
            close_step(step, outcome, std::chrono::steady_clock::now());
            if (!message.empty() && outcome == step_outcome::failed) {
                step.error_ = redactor_.text(message, error_bytes);
                bytes_ += text_detail::json_string_size(step.error_.view());
            } else if (!message.empty()) {
                ++log_total_;
                append_log(step, log_level::info, message, std::chrono::steady_clock::now());
            }
        } catch (...) {
            degraded_ = true;
        }
    }

    [[nodiscard]] std::string time_text(std::chrono::steady_clock::time_point at) const {
        const auto wall = system_started_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(at - started_);
        return wall_time_text(wall);
    }

    [[nodiscard]] static std::string wall_time_text(std::chrono::system_clock::time_point wall) {
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

    [[nodiscard]] std::string step_started_text(const step_record& step) const {
        return step.reported_started_at_ ? wall_time_text(*step.reported_started_at_) : time_text(step.started_);
    }

    [[nodiscard]] std::string summary_json() const {
        std::pmr::monotonic_buffer_resource pool;
        struct merged final {
            step_code code_;
            step_outcome outcome_;
            std::int64_t duration_ms_;
            std::int64_t call_count_;
            std::int64_t count_;
        };
        std::vector<merged> entries;
        for (const auto& step : steps_) {
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
        const auto failed = std::find_if(steps_.begin(), steps_.end(),
            [](const step_record& step) { return step.outcome_ == step_outcome::failed; });
        bool truncated = truncated_ || entries.size() > summary_steps;
        auto shown = std::min(entries.size(), summary_steps);
        while (true) {
            trace_summary_dto summary({.resource_ = &pool});
            summary.set<"state">("available");
            summary.set<"step_count">(static_cast<std::int64_t>(steps_.size() + omitted_steps_));
            summary.set<"call_count">(static_cast<std::int64_t>(call_total_));
            summary.set<"log_count">(static_cast<std::int64_t>(log_total_));
            summary.set<"truncated">(truncated);
            if (failed == steps_.end()) {
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

    void fill_pairs(ruvia::array<trace_pair_dto>& output, const std::vector<redacted_pair>& pairs,
        std::pmr::memory_resource* pool) const {
        for (const auto& pair : pairs) {
            auto& item = output.emplace_back(ruvia::model_options{.resource_ = pool});
            item.set<"name">(pair.name_.view());
            item.set<"value">(pair.value_.view());
            item.set<"redacted">(pair.redacted_);
        }
    }

    template <typename field_type>
    void fill_body(field_type&& assign, const std::optional<redacted_body>& body, bool keep,
        std::pmr::memory_resource* pool) const {
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

    [[nodiscard]] std::string document_json(reduction_level reduction) const {
        std::pmr::monotonic_buffer_resource pool;
        const bool keep_bodies = reduction == reduction_level::full;
        const bool keep_calls = reduction != reduction_level::without_calls;
        std::size_t omitted_calls = omitted_calls_;
        std::size_t omitted_logs = omitted_logs_;
        std::size_t omitted_body_bytes = omitted_body_bytes_;
        trace_document_dto document({.resource_ = &pool});
        document.set<"kind">(trace_kind_name(kind_));
        document.set<"worker_type">(worker_type_);
        auto& steps = document.ensure<"steps">();
        std::int64_t index = 0;
        for (const auto& step : steps_) {
            auto& item = steps.emplace_back(ruvia::model_options{.resource_ = &pool});
            const auto& descriptor = describe_step(step.code_);
            item.set<"index">(index++);
            item.set<"code">(descriptor.name_);
            item.set<"label">(descriptor.label_);
            item.set<"subject">(step.subject_.view());
            item.set<"started_at">(step_started_text(step));
            item.set<"offset_ms">(step.offset_ms_);
            item.set<"duration_ms">(step.duration_ms_);
            item.set<"outcome">(step_outcome_name(step.open_ ? step_outcome::interrupted : step.outcome_));
            item.set<"error">(step.error_.view());
            if (step.related_) {
                trace_related_task_dto related({.resource_ = &pool});
                related.set<"task_id">(step.related_->task_id_.view());
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
                output.set<"started_at">(time_text(call.started_));
                output.set<"offset_ms">(milliseconds_since(started_, call.started_));
                output.set<"duration_ms">(call.duration_ms_);
                output.set<"outcome">(call.outcome_);
                output.set<"repeat_count">(call.repeat_count_);
                output.set<"repeat_total_ms">(call.repeat_total_ms_);
                output.set<"last_started_at">(time_text(call.last_started_));
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
                output.set<"at">(time_text(log.at_));
                output.set<"offset_ms">(milliseconds_since(started_, log.at_));
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
            capture.set<"truncated">(truncated_ || reduction != reduction_level::full);
            capture.set<"degraded">(degraded());
            capture.set<"bytes">(bytes);
            capture.set<"omitted_steps">(static_cast<std::int64_t>(omitted_steps_));
            capture.set<"omitted_calls">(static_cast<std::int64_t>(omitted_calls));
            capture.set<"omitted_logs">(static_cast<std::int64_t>(omitted_logs));
            capture.set<"omitted_body_bytes">(static_cast<std::int64_t>(omitted_body_bytes));
            capture.set<"redactions">(static_cast<std::int64_t>(redactor_.redactions()));
            document.set<"capture">(std::move(capture));
            json = std::string(ruvia::to_json(document, {.resource_ = &pool}));
            if (static_cast<std::int64_t>(json.size()) == bytes) {
                break;
            }
            bytes = static_cast<std::int64_t>(json.size());
        }
        return json;
    }

    trace_kind kind_;
    std::string_view worker_type_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::system_clock::time_point system_started_;
    redactor redactor_;
    std::vector<step_record> steps_;
    std::vector<std::size_t> open_;
    std::optional<pending_call> pending_;
    std::optional<std::pair<std::size_t, std::size_t>> last_call_;
    std::uint64_t next_call_id_{};
    std::size_t bytes_{};
    std::size_t body_used_{};
    std::size_t stored_calls_{};
    std::size_t call_total_{};
    std::size_t log_total_{};
    std::size_t log_records_{};
    std::size_t log_bytes_{};
    std::size_t omitted_steps_{};
    std::size_t omitted_calls_{};
    std::size_t omitted_logs_{};
    std::size_t omitted_body_bytes_{};
    bool truncated_{};
    bool degraded_{};
    bool finished_{};
};

inline step_scope::~step_scope() {
    if (recorder_ != nullptr) {
        recorder_->end_step(index_, step_outcome::failed, {});
    }
}

inline void step_scope::complete() noexcept {
    if (auto* owner = std::exchange(recorder_, nullptr); owner != nullptr) {
        owner->end_step(index_, step_outcome::completed, {});
    }
}

inline void step_scope::skip(std::string_view reason) noexcept {
    if (auto* owner = std::exchange(recorder_, nullptr); owner != nullptr) {
        owner->end_step(index_, step_outcome::skipped, reason);
    }
}

inline void step_scope::fail(std::string_view error) noexcept {
    if (auto* owner = std::exchange(recorder_, nullptr); owner != nullptr) {
        owner->end_step(index_, step_outcome::failed, error);
    }
}

}  // namespace service::attempt_trace
