#include "node/runtime/config_activation.h"

#include <filesystem>
#include <fstream>

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
ruvia::task<void> activate_test_config(flexedge::node::state_store& store,
    flexedge::node::data_plane& data_plane,
    const TestConfig& config, flexedge::node::v2::ApplyPhase& phase) {
    flexedge::node::v2::DesiredState desired;
    *desired.mutable_node_spec() = config.active.node_spec();
    *desired.mutable_release() = config.active.release();
    flexedge::node::activate_desired_config(store, data_plane, desired, config.objects, phase);
    co_return;
}

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
    {
        ruvia::event_loop_pool workers({.loop_count_ = 1});
        ruvia::event_loop_pool control({.loop_count_ = 1});
        flexedge::node::runtime_state activation_runtime;
        flexedge::node::node_log_buffer activation_logs(1);
        flexedge::node::data_plane plane(workers, control.loop(0), activation_runtime, activation_logs);
        flexedge::node::state_store good_store(state_directory / "activation-good", "synthetic-test-secret");
        const auto bad_directory = state_directory / "activation-bad";
        std::filesystem::create_directories(bad_directory);
        std::ofstream(bad_directory / "active") << "not a directory";
        flexedge::node::state_store bad_store(bad_directory, "synthetic-test-secret");
        auto candidate = snapshot(false);
        candidate.active.mutable_node_spec()->mutable_content()->set_enabled(false);
        candidate.mutableWebsite()->set_https_enabled(false);
        for (auto& domain : *candidate.mutableWebsite()->mutable_domains()) {
            domain.set_https_enabled(false);
        }
        candidate.finalize();
        flexedge::node::v2::ApplyPhase phase{};
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        control.loop(0).start(activate_test_config(good_store, plane, candidate, phase)).get();
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
        bool rejected = false;
        try {
            control.loop(0).start(activate_test_config(bad_store, plane, candidate, phase)).get();
        } catch (const std::exception&) {
            rejected = true;
        }
        REQUIRE(rejected);
        REQUIRE(phase == flexedge::node::v2::APPLY_PHASE_ACTIVATE);
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
        control.loop(0).start(activate_test_config(good_store, plane, candidate, phase)).get();
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 4);
        const auto activated_reports = plane.origin_health_reports();
        REQUIRE(activated_reports.size() == 1);
        REQUIRE(activated_reports.front().website_id_ == "website-1");
        REQUIRE(activated_reports.front().origin_id_ == "origin-replacement");
        REQUIRE(request(candidate_port, "unknown.example.invalid").starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        candidate.setGeneration(3);
        candidate.finalize();
        bool stale_rejected = false;
        try {
            control.loop(0).start(activate_test_config(good_store, plane, candidate, phase)).get();
        } catch (const std::exception&) {
            stale_rejected = true;
        }
        REQUIRE(stale_rejected);
        REQUIRE(phase == flexedge::node::v2::APPLY_PHASE_VALIDATE);
        REQUIRE(activation_runtime.applied_node_spec_revision() == 4);
        REQUIRE(good_store.load().active_.node_spec().content().revision() == 4);
        REQUIRE(request(candidate_port, "unknown.example.invalid").starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        shutdown.stop_and_join();
        plane.request_stop_accepting();
        plane.request_stop_accepting();
    }
}
