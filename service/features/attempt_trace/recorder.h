#pragma once

#include <ruvia/http/http_client.h>
#include <ruvia/web/http_client_types.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/config/outbound.h"
#include "service/features/attempt_trace/mapper.h"
#include "service/features/attempt_trace/model.h"
#include "service/features/attempt_trace/outbound_policy.h"
#include "service/features/attempt_trace/redaction.h"
#include "service/utils/json_text.h"
#include "service/utils/text.h"

namespace service::attempt_trace {

// Another task this step waited for (a certificate waits for DNS sync).
// The linked task is the resource's task: (resource_type, resource_id).
struct related_task final {
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
            step.offset_ms_ = milliseconds_between(started_, step.started_);
            step.open_ = true;
            bytes_ += step_overhead + service::utils::json_string_size(step.subject_.view());
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
                    bytes_ += service::utils::json_string_size(step.error_.view());
                    return;
                }
            }
            if (!open_.empty() && open_.back() != dropped_step) {
                auto& step = steps_[open_.back()];
                step.error_ = redactor_.text(error, error_bytes);
                bytes_ += service::utils::json_string_size(step.error_.view());
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
            step.offset_ms_ = milliseconds_between(started_, step.started_);
            step.outcome_ = step_outcome::failed;
            step.error_ = redactor_.text(error, error_bytes);
            bytes_ += step_overhead + service::utils::json_string_size(step.error_.view());
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
            step.related_ = recorded_related{.resource_type_ = redactor_.text(task.resource_type_, identifier_bytes),
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
            if (call.policy_ == nullptr) {
                degraded_ = true;
                return {};
            }
            const auto& policy = *call.policy_;
            const auto& origin =
                service::config::outbound_origin_config(service::config::get_outbound_origins(), call.origin_alias_);
            const auto scheme = origin.scheme_ == ruvia::http_scheme::https ? std::string_view{"https"} : std::string_view{"http"};
            pending_call pending;
            pending.id_ = ++next_call_id_;
            pending.policy_ = call.policy_;
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
                if (service::utils::equals_ignore_case(header.name(), "content-type")) {
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
                if (service::utils::equals_ignore_case(header.name_, "content-type")) {
                    content_type = header.value_;
                }
                auto pair = redactor_.response_header(*pending.policy_, header.name_, header.value_);
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
                bytes_ += service::utils::json_string_size(call.error_.view());
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
            recorded_call record;
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
            bytes_ += step_overhead + service::utils::json_string_size(step.subject_.view()) +
                      service::utils::json_string_size(step.error_.view());
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
            recorded_step* by_phase = nullptr;
            recorded_step* by_time = nullptr;
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
                [](const recorded_step& left, const recorded_step& right) { return left.offset_ms_ < right.offset_ms_; });
            last_call_.reset();
            auto stored = to_stored_trace({.kind_ = kind_,
                .worker_type_ = worker_type_,
                .started_ = started_,
                .system_started_ = system_started_,
                .steps_ = steps_,
                .call_total_ = call_total_,
                .log_total_ = log_total_,
                .omitted_steps_ = omitted_steps_,
                .omitted_calls_ = omitted_calls_,
                .omitted_logs_ = omitted_logs_,
                .omitted_body_bytes_ = omitted_body_bytes_,
                .redactions_ = redactor_.redactions(),
                .truncated_ = truncated_,
                .degraded_ = degraded()});
            if (!stored) {
                truncated_ = true;
            }
            return stored;
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

    struct pending_call final {
        std::uint64_t id_{};
        const outbound_policy* policy_{};
        recorded_call record_;
        std::string query_name_;
        std::string query_type_;
    };

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
    void append_log(recorded_step& step, log_level level, std::string_view message,
        std::chrono::steady_clock::time_point at) {
        auto text = redactor_.text(message, log_message_bytes);
        if (!step.logs_.empty() && step.logs_.back().level_ == level && step.logs_.back().message_ == text) {
            ++step.logs_.back().repeat_count_;
            return;
        }
        const auto size = log_overhead + service::utils::json_string_size(text.view());
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
    recorded_step* current_step() {
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
        step.offset_ms_ = milliseconds_between(started_, step.started_);
        step.outcome_ = step_outcome::completed;
        bytes_ += step_overhead;
        return &step;
    }

    [[nodiscard]] redacted_body capture_body(const outbound_policy& policy, bool request, std::string_view content_type,
        std::string_view body, std::size_t limit) {
        const auto budget = body_used_ < body_budget_bytes ? body_budget_bytes - body_used_ : 0;
        auto result = redactor_.body(policy, request, content_type, body, limit, budget);
        if (result.omitted_reason_.empty()) {
            body_used_ += service::utils::json_string_size(result.excerpt_.view());
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
            size += pair_overhead + service::utils::json_string_size(pair.name_.view()) +
                    service::utils::json_string_size(pair.value_.view());
        }
        return size;
    }

    [[nodiscard]] static std::size_t body_size(const std::optional<redacted_body>& body) noexcept {
        return body ? body_overhead + service::utils::json_string_size(body->excerpt_.view()) +
                          service::utils::json_string_size(body->content_type_.view())
                    : 0;
    }

    [[nodiscard]] static std::size_t call_size(const recorded_call& record) noexcept {
        auto size = call_overhead + service::utils::json_string_size(record.subject_.view()) +
                    service::utils::json_string_size(record.provider_request_id_.view()) +
                    service::utils::json_string_size(record.provider_code_.view()) +
                    service::utils::json_string_size(record.error_.view());
        if (record.http_) {
            const auto& http = *record.http_;
            size += service::utils::json_string_size(http.url_.url_.view()) * 2 + pairs_size(http.url_.query_) +
                    pairs_size(http.request_headers_) + pairs_size(http.response_headers_) +
                    body_size(http.request_body_) + body_size(http.response_body_) +
                    service::utils::json_string_size(http.host_.view());
        }
        if (record.dns_) {
            size += dns_overhead + service::utils::json_string_size(record.dns_->name_.view());
            for (const auto& answer : record.dns_->answers_) {
                size += answer_overhead + service::utils::json_string_size(answer.name_.view()) +
                        service::utils::json_string_size(answer.data_.view());
            }
        }
        if (record.node_) {
            size += dns_overhead + service::utils::json_string_size(record.node_->request_id_.view());
            for (const auto& field : record.node_->fields_) {
                size += pair_overhead + service::utils::json_string_size(field.name_.view()) +
                        service::utils::json_string_size(field.value_.view());
            }
        }
        return size;
    }

    // Two calls are the same request when everything shown about them
    // except the time matches (ACME polling, nonce fetches).
    [[nodiscard]] static bool same_call(const recorded_call& left, const recorded_call& right) noexcept {
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

    void store_call(recorded_call record) {
        const auto now = std::chrono::steady_clock::now();
        record.duration_ms_ = record.duration_ms_override_.value_or(milliseconds_between(record.started_, now));
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

    void omit_call(recorded_step& step) noexcept {
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

    void close_step(recorded_step& step, step_outcome outcome, std::chrono::steady_clock::time_point now) noexcept {
        if (!step.open_) {
            return;
        }
        step.open_ = false;
        step.outcome_ = outcome;
        step.duration_ms_ = milliseconds_between(step.started_, now);
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
                bytes_ += service::utils::json_string_size(step.error_.view());
            } else if (!message.empty()) {
                ++log_total_;
                append_log(step, log_level::info, message, std::chrono::steady_clock::now());
            }
        } catch (...) {
            degraded_ = true;
        }
    }

    trace_kind kind_;
    std::string_view worker_type_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::system_clock::time_point system_started_;
    redactor redactor_;
    std::vector<recorded_step> steps_;
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
