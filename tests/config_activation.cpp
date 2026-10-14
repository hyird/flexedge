#include "node/runtime/config_activation.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "node/runtime/apply_trace.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)
using flexedge::testing::request;
using flexedge::testing::snapshot;
using flexedge::testing::TestConfig;
using flexedge::testing::waitUntil;
namespace v2 = flexedge::node::v2;

namespace {

constexpr std::string_view state_secret = "synthetic-test-secret";

std::int64_t unix_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ruvia::task<v2::ApplyResult> apply_test_config(flexedge::node::state_store& store,
    flexedge::node::data_plane& data_plane, const TestConfig& config) {
    v2::DesiredState desired;
    *desired.mutable_node_spec() = config.active.node_spec();
    *desired.mutable_release() = config.active.release();
    flexedge::node::apply_trace trace;
    v2::ApplyResult result;
    (void)flexedge::node::apply_desired_config(store, data_plane, desired, config.objects, trace, result);
    co_return result;
}

std::vector<std::string> phases(const v2::ApplyResult& result) {
    std::vector<std::string> names;
    for (const auto& step : result.steps()) {
        names.push_back(step.phase());
    }
    return names;
}

bool logged(const v2::ApplyResult& result, std::string_view level, std::string_view text) {
    for (const auto& line : result.logs()) {
        if (line.level() == level && line.message().find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Steps and logs carry timing, fixed wording, counts and the reused ApplyResult.error only.
void require_trace_shape(const v2::ApplyResult& result, const TestConfig& config,
    std::int64_t started_unix_ms, std::int64_t finished_unix_ms) {
    std::vector<std::string> forbidden{std::string(state_secret), "private-key", "-----BEGIN",
        config.active.release().digest_sha256()};
    for (const auto& object : config.objects) {
        forbidden.push_back(object.digest_sha256());
    }
    const auto require_clean = [&](const std::string& text) {
        REQUIRE(text.size() <= flexedge::node::apply_trace::maximum_text_bytes);
        for (const auto& value : forbidden) {
            REQUIRE(value.empty() || text.find(value) == std::string::npos);
        }
    };
    REQUIRE(!result.steps().empty() && result.steps_size() <= 16 && result.logs_size() <= 100);
    for (const auto& step : result.steps()) {
        REQUIRE(step.started_unix_ms() >= started_unix_ms && step.started_unix_ms() <= finished_unix_ms);
        REQUIRE(step.duration_ms() >= 0);
        REQUIRE(step.ok() == step.error().empty());
        REQUIRE(step.ok() || step.error() == result.error());
        require_clean(step.phase());
        require_clean(step.error());
    }
    for (const auto& line : result.logs()) {
        REQUIRE(line.level() == "info" || line.level() == "warning" || line.level() == "error");
        REQUIRE(line.unix_ms() >= started_unix_ms && line.unix_ms() <= finished_unix_ms);
        require_clean(line.message());
        if (line.level() == "info") {
            REQUIRE(line.message().find("revision") == std::string::npos);
            REQUIRE(line.message().find("generation") == std::string::npos);
        }
    }
    // Each line names the step it summarizes: logged after that step ended, it must not
    // attach to the following step on the controller.
    const auto expected_phase = [&](const std::string& message) -> std::string {
        if (message.starts_with("staged ")) {
            return "stage";
        }
        if (message.starts_with("compiled ")) {
            return "validate";
        }
        if (message.starts_with("prepared listeners")) {
            return "prepare_listeners";
        }
        if (message.starts_with("activated ")) {
            return "activate";
        }
        REQUIRE(message.starts_with("apply failed: "));
        return result.steps(result.steps_size() - 1).phase();
    };
    for (const auto& line : result.logs()) {
        REQUIRE(line.phase() == expected_phase(line.message()));
    }
    if (result.applied()) {
        REQUIRE(result.logs_size() == 0 ||
                result.logs(result.logs_size() - 1).message() == "activated the configuration");
    } else {
        REQUIRE(!result.steps(result.steps_size() - 1).ok());
        REQUIRE(logged(result, "error", "apply failed: " + result.error()));
    }
}

void verify_trace_limits() {
    using flexedge::node::apply_log_level;
    using flexedge::node::apply_trace;
    using flexedge::node::truncate_utf8;
    // "a" then two-byte characters: byte 512 starts a character, byte 511 continues one.
    std::string wide("a");
    for (int index = 0; index < 300; ++index) {
        wide.append("\xC3\xA9");
    }
    REQUIRE(truncate_utf8(wide, 512).size() == 511);
    REQUIRE(truncate_utf8("short", 512) == "short");
    REQUIRE(truncate_utf8(wide.substr(1), 512).size() == 512);

    apply_trace trace;
    trace.log(apply_log_level::info, "before any step");
    for (int index = 0; index < 20; ++index) {
        trace.run_step("stage", [] {});
    }
    trace.log(apply_log_level::info, "after stage");
    const auto open = trace.begin_step("fetch_objects");
    trace.log(apply_log_level::info, "during fetch");
    trace.end_step(open, true);
    bool rethrown = false;
    try {
        trace.run_step("activate", [&]() -> int { throw std::runtime_error(wide); });
    } catch (const std::runtime_error&) {
        rethrown = true;
    }
    REQUIRE(rethrown);
    for (int index = 0; index < 147; ++index) {
        trace.log(apply_log_level::info, wide);
    }
    v2::ApplyResult result;
    trace.write_to(result);
    REQUIRE(result.logs(0).message() == "before any step" && result.logs(0).phase().empty());
    REQUIRE(result.logs(1).message() == "after stage" && result.logs(1).phase() == "stage");
    REQUIRE(result.logs(2).message() == "during fetch" && result.logs(2).phase() == "fetch_objects");
    REQUIRE(result.logs(3).phase() == "activate");
    REQUIRE(result.steps_size() == 16);
    REQUIRE(result.logs_size() == 100);
    REQUIRE(result.logs(3).message().size() == 511);
    REQUIRE(result.logs(98).message() == truncate_utf8(wide, 512));
    REQUIRE(result.logs(99).level() == "warning");
    REQUIRE(result.logs(99).message() == "51 further log lines omitted");
    REQUIRE(result.logs(99).phase().empty());

    apply_trace failed;
    try {
        failed.run_step("validate", [&] { throw std::runtime_error(wide); });
    } catch (const std::runtime_error&) {
    }
    failed.write_to(result);
    REQUIRE(result.steps_size() == 1 && result.logs_size() == 0);
    REQUIRE(!result.steps(0).ok() && result.steps(0).error().size() == 511);
}

}  // namespace

int main() {
    const auto state_directory =
        std::filesystem::temp_directory_path() /
        ("flexedge-node-state-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct state_directory_cleanup final {
        std::filesystem::path path_;
        ~state_directory_cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    } state_cleanup{state_directory};
    verify_trace_limits();
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        flexedge::node::runtime_state activation_runtime;
        flexedge::node::node_log_buffer activation_logs(1);
        flexedge::node::data_plane plane(workers, control.loop(0), activation_runtime, activation_logs);
        flexedge::node::state_store good_store(state_directory / "activation-good", std::string(state_secret));
        const auto bad_directory = state_directory / "activation-bad";
        std::filesystem::create_directories(bad_directory);
        std::ofstream(bad_directory / "active") << "not a directory";
        flexedge::node::state_store bad_store(bad_directory, std::string(state_secret));
        auto candidate = snapshot(false);
        candidate.active.mutable_node_spec()->mutable_content()->set_enabled(false);
        candidate.mutableWebsite()->set_https_enabled(false);
        for (auto& domain : *candidate.mutableWebsite()->mutable_domains()) {
            domain.set_https_enabled(false);
        }
        candidate.finalize();
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        const auto apply = [&](flexedge::node::state_store& store, const TestConfig& config) {
            const auto started = unix_ms_now();
            auto result = control.loop(0).start(apply_test_config(store, plane, config)).get();
            require_trace_shape(result, config, started, unix_ms_now());
            return result;
        };

        const auto applied = apply(good_store, candidate);
        REQUIRE(applied.applied());
        REQUIRE((phases(applied) == std::vector<std::string>{"stage", "validate", "prepare_listeners", "activate"}));
        REQUIRE(applied.logs_size() == 4);
        REQUIRE(logged(applied, "info", "staged the release state"));
        REQUIRE(logged(applied, "info", "compiled the configuration: 1 websites, 1 endpoints, node disabled"));
        REQUIRE(logged(applied, "info", "prepared listeners: 0 new, 0 retired"));
        REQUIRE(activation_runtime.applied_node_spec_revision() == 3);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 3);
        const auto original_reports = plane.origin_health_reports();
        REQUIRE(original_reports.size() == 1);
        REQUIRE(original_reports.front().website_id_ == "website-1");
        REQUIRE(original_reports.front().origin_id_ == "origin-1");
        candidate.setGeneration(4);
        candidate.mutableWebsite()->mutable_origins(0)->set_id("origin-replacement");
        // Prepare a real listener before persistence fails. A successful
        // exclusive rebind afterwards proves abort released the socket.
        asio::io_context reservation_context;
        asio::ip::tcp::acceptor reservation(reservation_context,
            asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
        const auto candidate_port = reservation.local_endpoint().port();
        reservation.close();
        candidate.active.mutable_node_spec()->mutable_content()->set_enabled(true);
        auto* candidate_endpoint = candidate.active.mutable_node_spec()->mutable_content()->mutable_endpoints(0);
        candidate_endpoint->set_http_port(candidate_port);
        candidate_endpoint->set_https_port(candidate_port == 443 ? 444 : 443);
        candidate.finalize();
        const auto persist_failed = apply(bad_store, candidate);
        REQUIRE(!persist_failed.applied());
        REQUIRE(persist_failed.failed_phase() == v2::APPLY_PHASE_ACTIVATE);
        REQUIRE(persist_failed.error_code() == "apply_failed" && persist_failed.retryable());
        REQUIRE((phases(persist_failed) == std::vector<std::string>{"stage", "validate", "prepare_listeners", "activate"}));
        REQUIRE(logged(persist_failed, "info", "prepared listeners: 1 new, 0 retired"));
        REQUIRE(activation_runtime.applied_node_spec_revision() == 3);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 3);
        const auto rolled_back_reports = plane.origin_health_reports();
        REQUIRE(rolled_back_reports.size() == 1);
        REQUIRE(rolled_back_reports.front().website_id_ == "website-1");
        REQUIRE(rolled_back_reports.front().origin_id_ == "origin-1");
        REQUIRE(waitUntil([&] {
            asio::ip::tcp::acceptor probe(reservation_context);
            std::error_code error;
            probe.open(asio::ip::tcp::v4(), error);
            if (error) {
                return false;
            }
            probe.bind({asio::ip::address_v4::any(), candidate_port}, error);
            return !error;
        },
            std::chrono::seconds(2)));
        const auto activated = apply(good_store, candidate);
        REQUIRE(activated.applied());
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 4);
        const auto activated_reports = plane.origin_health_reports();
        REQUIRE(activated_reports.size() == 1);
        REQUIRE(activated_reports.front().website_id_ == "website-1");
        REQUIRE(activated_reports.front().origin_id_ == "origin-replacement");
        REQUIRE(request(candidate_port, "unknown.example.invalid").starts_with("HTTP/1.1 421 Misdirected Request\r\n"));

        // A port another process holds fails while binding the new listener, before activation.
        asio::ip::tcp::acceptor occupied(reservation_context,
            asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
        auto blocked = candidate;
        blocked.setGeneration(5);
        blocked.active.mutable_node_spec()->mutable_content()->mutable_endpoints(0)->set_http_port(
            occupied.local_endpoint().port());
        blocked.finalize();
        const auto bind_failed = apply(good_store, blocked);
        REQUIRE(!bind_failed.applied());
        REQUIRE(bind_failed.failed_phase() == v2::APPLY_PHASE_VALIDATE);
        REQUIRE((phases(bind_failed) == std::vector<std::string>{"stage", "validate", "prepare_listeners"}));
        REQUIRE(bind_failed.steps(2).error().find("could not start edge HTTP listener") != std::string::npos);
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(request(candidate_port, "unknown.example.invalid").starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        occupied.close();

        candidate.setGeneration(3);
        candidate.finalize();
        const auto stale = apply(good_store, candidate);
        REQUIRE(!stale.applied());
        REQUIRE(stale.failed_phase() == v2::APPLY_PHASE_VALIDATE);
        REQUIRE(stale.error_code() == "artifact_invalid" && !stale.retryable());
        // A release older than the active one is rejected while preparing, before any listener.
        REQUIRE((phases(stale) == std::vector<std::string>{"stage", "validate", "prepare_listeners"}));
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 4);
        // A configuration that does not compile fails in the validate step itself.
        auto invalid = candidate;
        invalid.setGeneration(6);
        invalid.duplicateWebsite();
        invalid.finalize();
        const auto invalid_result = apply(good_store, invalid);
        REQUIRE(!invalid_result.applied());
        REQUIRE(invalid_result.failed_phase() == v2::APPLY_PHASE_VALIDATE);
        REQUIRE(invalid_result.error_code() == "artifact_invalid" && !invalid_result.retryable());
        REQUIRE((phases(invalid_result) == std::vector<std::string>{"stage", "validate"}));
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(request(candidate_port, "unknown.example.invalid").starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        shutdown.stop_and_join();
        plane.request_stop_accepting();
        plane.request_stop_accepting();
    }
}
