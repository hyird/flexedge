#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/model_object.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "service/features/attempt_trace/recorder.h"
#include "service/features/certificate/acme_transport.h"
#include "service/features/dns/cloudflare.h"
#include "support/attempt_trace_fixture.h"

namespace {

using service::attempt_trace::log_level;
using service::attempt_trace::outbound_policy;
namespace acme_trace = service::certificate_issuance::acme_trace;
using service::attempt_trace::recorder;
using service::attempt_trace::step_code;
using test_support::attempt_trace::exchange;
using test_support::attempt_trace::perform;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("attempt trace recorder: " + std::string(message));
    }
}

RUVIA_MODEL(call_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(operation, ruvia::string),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string), RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(repeat_total_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(started_at, ruvia::string),
    RUVIA_REQUIRED_FIELD(last_started_at, ruvia::string), RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(http, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(node_message, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(log_input, RUVIA_REQUIRED_FIELD(level, ruvia::string), RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64));
RUVIA_MODEL(step_input, RUVIA_REQUIRED_FIELD(index, ruvia::int64), RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(label, ruvia::string), RUVIA_REQUIRED_FIELD(subject, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(related_task, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(calls, ruvia::array<call_input>), RUVIA_REQUIRED_FIELD(logs, ruvia::array<log_input>));
RUVIA_MODEL(capture_input, RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(degraded, ruvia::bool_value), RUVIA_REQUIRED_FIELD(bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_steps, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_logs, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_body_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(redactions, ruvia::int64));
RUVIA_MODEL(document_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(worker_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(capture, capture_input), RUVIA_REQUIRED_FIELD(steps, ruvia::array<step_input>));
RUVIA_MODEL(summary_step_input, RUVIA_REQUIRED_FIELD(code, ruvia::string), RUVIA_REQUIRED_FIELD(label, ruvia::string),
    RUVIA_REQUIRED_FIELD(outcome, ruvia::string), RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(count, ruvia::int64));
RUVIA_MODEL(failed_step_input, RUVIA_REQUIRED_FIELD(code, ruvia::string), RUVIA_REQUIRED_FIELD(label, ruvia::string));
RUVIA_MODEL(summary_input, RUVIA_REQUIRED_FIELD(state, ruvia::string), RUVIA_REQUIRED_FIELD(step_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(log_count, ruvia::int64),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(failed_step, failed_step_input, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(steps, ruvia::array<summary_step_input>));

struct finished final {
    document_input document_;
    summary_input summary_;
    std::size_t bytes_{};
};

finished finish(recorder& trace) {
    auto stored = trace.finish();
    require(stored.has_value(), "trace serializes");
    require(stored->trace_json_.size() <= service::attempt_trace::trace_max_bytes, "trace byte cap");
    require(static_cast<std::size_t>(stored->trace_bytes_) == stored->trace_json_.size(), "stored size");
    require(stored->summary_json_.size() <= service::attempt_trace::summary_max_bytes, "summary byte cap");
    auto document = ruvia::from_json<document_input>(stored->trace_json_);
    auto summary = ruvia::from_json<summary_input>(stored->summary_json_);
    require(document.has_value() && summary.has_value(), "trace and summary match their contract");
    require(document->get<"capture">().get<"bytes">().value_ == static_cast<std::int64_t>(stored->trace_json_.size()),
        "capture reports the stored size");
    require(!trace.finish().has_value(), "a recorder finishes once");
    return {std::move(*document), std::move(*summary), stored->trace_json_.size()};
}

recorder new_recorder() {
    return recorder(service::attempt_trace::trace_kind::certificate, "certificate", std::chrono::steady_clock::now());
}

exchange acme_call(const outbound_policy& policy, std::string target, std::string body) {
    return {.call_ = {.origin_alias_ = service::config::lets_encrypt_origin_alias, .policy_ = &policy},
        .method_ = &policy == &acme_trace::new_nonce ? "HEAD" : "POST",
        .target_ = std::move(target),
        .status_ = 200,
        .response_headers_ = {{"content-type", "application/json"}, {"replay-nonce", "nonce-value-123456"}},
        .response_body_ = std::move(body)};
}

void verify_steps_failures_and_folding() {
    auto trace = new_recorder();
    {
        auto step = trace.begin_step(step_code::prepare);
        trace.log(log_level::info, "prepared");
        step.complete();
    }
    {
        auto step = trace.begin_step(step_code::validation, "www.example.com");
        for (int poll = 0; poll < 5; ++poll) {
            perform(trace, acme_call(acme_trace::new_nonce, "/acme/new-nonce", ""));
            perform(trace, acme_call(acme_trace::authorization, "/acme/authz/1", R"({"status":"pending"})"));
            trace.log(log_level::info, "授权状态 pending");
        }
        perform(trace, acme_call(acme_trace::authorization, "/acme/authz/1", R"({"status":"invalid"})"));
        trace.mark_invalid_response("authorization became invalid");
        // Left without complete(): an exception unwinding records a failure.
    }
    {
        auto step = trace.begin_step(step_code::cleanup);
        step.skip("没有写入 DNS 验证记录，无需清理");
    }
    trace.mark_failure("ACME DNS-01 验证失败");
    trace.link_task({.resource_type_ = "dns_zone", .resource_id_ = "r", .label_ = "DNS 同步"});
    {
        auto step = trace.begin_step(step_code::record_failure);
        trace.link_task({.resource_type_ = "dns_zone",
            .resource_id_ = "00000000-0000-4000-8000-000000000002",
            .label_ = "DNS 同步"});
        // Still open when the result commits: finish() completes it.
        const auto result = finish(trace);
        const auto& steps = result.document_.get<"steps">();
        require(steps.size() == 4, "four steps");
        require(steps[0].get<"code">().view() == "prepare" && steps[0].get<"label">().view() == "准备签发" &&
                    steps[0].get<"outcome">().view() == "completed" && steps[0].get<"logs">().size() == 1,
            "completed step with its log");
        const auto& validation = steps[1];
        require(validation.get<"outcome">().view() == "failed" &&
                    validation.get<"error">().view() == "ACME DNS-01 验证失败" &&
                    validation.get<"subject">().view() == "www.example.com",
            "unwound step failed and carries the attempt error");
        require(validation.get<"call_count">().value_ == 11, "every call counted");
        const auto& calls = validation.get<"calls">();
        require(calls.size() == 3 && calls[0].get<"repeat_count">().value_ == 5 &&
                    calls[1].get<"repeat_count">().value_ == 5 &&
                    calls[0].get<"operation">().view() == "acme.new_nonce",
            "alternating polls fold into two calls");
        require(calls[0].get<"started_at">().view() <= calls[0].get<"last_started_at">().view(),
            "folded call keeps first and last start");
        require(calls[2].get<"outcome">().view() == "invalid_response" &&
                    calls[2].get<"error">().view() == "authorization became invalid",
            "invalid response marked");
        require(validation.get<"logs">().size() == 1 && validation.get<"logs">()[0].get<"repeat_count">().value_ == 5,
            "identical lines fold");
        require(steps[2].get<"outcome">().view() == "skipped" && steps[2].get<"logs">().size() == 1,
            "skipped step with its reason");
        require(steps[3].get<"outcome">().view() == "completed" && steps[3].get<"related_task">().has_value(),
            "open step completed by the commit; related task linked");
        for (std::size_t index = 1; index < steps.size(); ++index) {
            require(steps[index].get<"offset_ms">().value_ >= steps[index - 1].get<"offset_ms">().value_,
                "steps in start order");
            require(steps[index].get<"started_at">().view().ends_with("+00"), "UTC timestamps");
        }
        const auto& summary = result.summary_;
        require(summary.get<"state">().view() == "available" && summary.get<"step_count">().value_ == 4 &&
                    summary.get<"call_count">().value_ == 11 && summary.get<"log_count">().value_ == 7,
            "summary counts");
        require(summary.get<"failed_step">().has_value() && summary.get<"failed_step">()->get<"code">().view() == "validation",
            "summary names the failed step");
        require(summary.get<"steps">().size() == 4 && summary.get<"steps">()[1].get<"call_count">().value_ == 11,
            "summary steps");
        require(result.document_.get<"kind">().view() == "certificate" &&
                    result.document_.get<"worker_type">().view() == "certificate",
            "kind and worker type");
        step.complete();
    }
}

void verify_bounds() {
    auto trace = recorder(service::attempt_trace::trace_kind::dns_sync, "dns-sync", std::chrono::steady_clock::now());
    const std::string large_body(20000, 'y');
    {
        auto step = trace.begin_step(step_code::reconcile_records);
        for (int index = 0; index < 150; ++index) {
            perform(trace, {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                                .policy_ = &service::dns::cloudflare_trace::update_record,
                                .subject_ = "host-" + std::to_string(index) + " A"},
                               .method_ = "PUT",
                               .target_ = "/client/v4/zones/z/dns_records/" + std::to_string(index),
                               .request_headers_ = {{"content-type", "application/json"}},
                               .request_body_ = R"({"type":"A","name":"host","content":"192.0.2.)" +
                                                std::to_string(index % 250) + R"(","ttl":300})",
                               .has_request_body_ = true,
                               .status_ = 400,
                               .response_headers_ = {{"content-type", "text/plain"}},
                               .response_body_ = large_body});
        }
        for (int index = 0; index < 400; ++index) {
            trace.log(log_level::info, "line " + std::to_string(index) + std::string(100, '.'));
        }
        step.complete();
    }
    for (int index = 0; index < 140; ++index) {
        auto step = trace.begin_step(step_code::delete_obsolete, "record " + std::to_string(index));
        step.complete();
    }
    const auto result = finish(trace);
    const auto& capture = result.document_.get<"capture">();
    const auto& reconcile = result.document_.get<"steps">()[0];
    require(reconcile.get<"call_count">().value_ == 150 && reconcile.get<"calls">().size() <= 100 &&
                reconcile.get<"omitted_calls">().value_ >= 50,
        "per-step call cap with exact count");
    require(capture.get<"truncated">().value_ && capture.get<"omitted_calls">().value_ >= 50 &&
                capture.get<"omitted_logs">().value_ >= 100 && capture.get<"omitted_body_bytes">().value_ > 0 &&
                capture.get<"omitted_steps">().value_ == 13,
        "omissions counted");
    require(result.document_.get<"steps">().size() == service::attempt_trace::max_steps, "step cap");
    require(result.bytes_ <= service::attempt_trace::trace_max_bytes, "byte cap");
    require(result.summary_.get<"steps">().size() <= service::attempt_trace::summary_steps &&
                result.summary_.get<"truncated">().value_ && result.summary_.get<"steps">()[1].get<"count">().value_ > 1,
        "summary merges adjacent steps and stays bounded");
}

void verify_failures_never_escape() {
    auto trace = new_recorder();
    {
        auto step = trace.begin_step(step_code::order);
        // An origin that is not configured: capture fails, the attempt does not.
        perform(trace, {.call_ = {.origin_alias_ = "not-configured", .policy_ = &acme_trace::new_order},
                           .method_ = "POST",
                           .target_ = "/acme/new-order",
                           .status_ = 200});
        trace.log(log_level::info, "ignored after degradation");
        step.complete();
    }
    const auto result = finish(trace);
    require(result.document_.get<"capture">().get<"degraded">().value_, "degraded capture is reported");
    require(result.document_.get<"steps">().size() == 1, "what was captured before degradation remains");

    auto unscoped = new_recorder();
    perform(unscoped, acme_call(acme_trace::directory, "/directory", R"({"newNonce":"x"})"));
    unscoped.log(log_level::info, "outside any step");
    unscoped.mark_failure("failure outside any step");
    const auto outside = finish(unscoped);
    const auto& steps = outside.document_.get<"steps">();
    require(steps.size() == 2 && steps[0].get<"code">().view() == "unscoped" && steps[0].get<"calls">().size() == 1 &&
                steps[1].get<"outcome">().view() == "failed" &&
                steps[1].get<"error">().view() == "failure outside any step",
        "calls and failures outside steps are kept");

    auto pending = new_recorder();
    {
        auto step = pending.begin_step(step_code::download);
        const std::string target = "/acme/cert/1";
        const ruvia::http_client_request_view request{.method_ = "POST", .target_ = target};
        (void)pending.begin_http({.origin_alias_ = service::config::lets_encrypt_origin_alias,
                                     .policy_ = &acme_trace::certificate},
            request);
        const auto cancelled = finish(pending);
        require(cancelled.document_.get<"steps">()[0].get<"calls">()[0].get<"outcome">().view() == "cancelled",
            "an unfinished call is recorded as cancelled");
    }
}

void verify_reported_steps() {
    auto trace = recorder(service::attempt_trace::trace_kind::node_apply, "web",
        std::chrono::steady_clock::now() - std::chrono::milliseconds(200));
    const auto now = std::chrono::system_clock::now();
    const auto prepare_started = now - std::chrono::milliseconds(50);
    trace.record_reported_step(step_code::validate_config, "validate", "", now - std::chrono::milliseconds(100), 12,
        true, {});
    trace.record_reported_step(service::attempt_trace::node_phase_step("prepare_listeners"), "prepare_listeners", "",
        prepare_started, 3, false, "port 443 already bound");
    // A node writes a step's summary after the step ends, here when the next
    // step has started: the phase, not the time, decides.
    trace.record_reported_log(log_level::info, prepare_started, "validate", "compiled the configuration: 2 websites");
    trace.record_reported_log(log_level::info, prepare_started + std::chrono::milliseconds(20), "validate",
        "validation summary after the end of validate");
    // Without a phase (or naming no recorded step) the time rule applies.
    trace.record_reported_log(log_level::info, prepare_started + std::chrono::milliseconds(1), "", "no phase");
    trace.record_reported_log(log_level::info, now - std::chrono::milliseconds(99), "fetch_objects", "unknown phase");
    trace.record_reported_log(log_level::error, prepare_started + std::chrono::milliseconds(4), "prepare_listeners",
        "apply failed: port 443 already bound");
    const service::attempt_trace::node_message_field fields[]{{.name_ = "applied", .value_ = "false"}};
    {
        auto step = trace.begin_step(step_code::record_result);
        trace.record_node_message({.direction_ = "received",
                                      .message_ = "ApplyResult",
                                      .request_id_ = "release-probe-3",
                                      .object_count_ = 0,
                                      .bytes_ = 120,
                                      .fields_ = fields},
            std::chrono::steady_clock::now());
        step.complete();
    }
    const auto result = finish(trace);
    const auto& steps = result.document_.get<"steps">();
    require(steps.size() == 3 && steps[0].get<"code">().view() == "validate_config" &&
                steps[0].get<"duration_ms">().value_ == 12 &&
                steps[1].get<"code">().view() == "prepare_listeners" && steps[1].get<"outcome">().view() == "failed" &&
                steps[1].get<"error">().view() == "port 443 already bound" &&
                steps[2].get<"code">().view() == "record_result" && result.document_.get<"kind">().view() == "node_apply",
        "reported steps keep their own timing and errors, in start order");
    const auto messages = [&](std::size_t index) {
        std::vector<std::string> values;
        for (const auto& line : steps[index].get<"logs">()) {
            values.emplace_back(line.get<"message">().view());
        }
        return values;
    };
    require(messages(0) == std::vector<std::string>{"compiled the configuration: 2 websites",
                               "validation summary after the end of validate", "unknown phase"},
        "lines attach to the step of their phase even when their time ties with or follows the next step");
    require(messages(1) == std::vector<std::string>{"no phase", "apply failed: port 443 already bound"},
        "lines without a known phase attach by time");
    require(steps[2].get<"calls">().size() == 1 && steps[2].get<"calls">()[0].get<"kind">().view() == "node_message" &&
                steps[2].get<"calls">()[0].get<"node_message">().has_value(),
        "node message recorded");
}

}  // namespace

int main() {
    try {
        test_support::attempt_trace::ensure_outbound_origins();
        verify_steps_failures_and_folding();
        verify_bounds();
        verify_failures_never_escape();
        verify_reported_steps();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Attempt trace recorder passed\n";
    return 0;
}
