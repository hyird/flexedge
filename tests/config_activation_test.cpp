#include <filesystem>
#include <fstream>
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error("requirement failed: " #condition); } while(false)
using flexedge::testing::TestConfig;
using flexedge::testing::snapshot;
using flexedge::testing::request;
using flexedge::testing::waitUntil;
ruvia::Task<void> activateTestConfig(flexedge::node::StateStore& store,
    flexedge::node::DataPlane& dataPlane,
    const TestConfig& config, flexedge::node::v2::ApplyPhase& phase) {
    flexedge::node::v2::DesiredState desired;
    *desired.mutable_node_spec() = config.active.node_spec();
    *desired.mutable_release() = config.active.release();
    flexedge::node::activateDesiredConfig(store, dataPlane, desired, config.objects, phase);
    co_return;
}


int main() {
        const auto stateDirectory =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-state-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct StateDirectoryCleanup final {
            std::filesystem::path path;
            ~StateDirectoryCleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } stateCleanup{stateDirectory};
        {
            ruvia::EventLoopPool workers({.loopCount = 1});
            ruvia::EventLoopPool control({.loopCount = 1});
            flexedge::node::RuntimeState activationRuntime;
            flexedge::node::NodeLogBuffer activationLogs(1);
            flexedge::node::DataPlane plane(workers, control.loop(0), activationRuntime, activationLogs);
            flexedge::node::StateStore goodStore(stateDirectory / "activation-good", "synthetic-test-secret");
            const auto badDirectory = stateDirectory / "activation-bad";
            std::filesystem::create_directories(badDirectory);
            std::ofstream(badDirectory / "active") << "not a directory";
            flexedge::node::StateStore badStore(badDirectory, "synthetic-test-secret");
            auto candidate = snapshot(false);
            candidate.active.mutable_node_spec()->mutable_content()->set_enabled(false);
            candidate.mutableWebsite()->set_https_enabled(false);
            for (auto& domain : *candidate.mutableWebsite()->mutable_domains())
                domain.set_https_enabled(false);
            candidate.finalize();
            flexedge::node::v2::ApplyPhase phase{};
            flexedge::node::LoopShutdown shutdown(workers, control);
            workers.start();
            control.start();
            control.loop(0).start(activateTestConfig(goodStore, plane, candidate, phase)).get();
            REQUIRE(activationRuntime.appliedNodeSpecRevision() == 3);
            REQUIRE(goodStore.load().active.node_spec().content().revision() == 3);
            const auto originalReports = plane.originHealthReports();
            REQUIRE(originalReports.size() == 1);
            REQUIRE(originalReports.front().websiteId == "website-1");
            REQUIRE(originalReports.front().originId == "origin-1");
            candidate.setGeneration(4);
            candidate.mutableWebsite()->mutable_origins(0)->set_id("origin-replacement");
            // Prepare a real listener before persistence fails. A successful
            // exclusive rebind afterwards proves abort released the socket.
            asio::io_context reservationContext;
            asio::ip::tcp::acceptor reservation(reservationContext,
                asio::ip::tcp::endpoint(asio::ip::address_v4::any(), 0));
            const auto candidatePort = reservation.local_endpoint().port();
            reservation.close();
            candidate.active.mutable_node_spec()->mutable_content()->set_enabled(true);
            auto* candidateEndpoint = candidate.active.mutable_node_spec()->mutable_content()->mutable_endpoints(0);
            candidateEndpoint->set_http_port(candidatePort);
            candidateEndpoint->set_https_port(candidatePort == 443 ? 444 : 443);
            candidate.finalize();
            bool rejected = false;
            try {
                control.loop(0).start(activateTestConfig(badStore, plane, candidate, phase)).get();
            } catch (const std::exception&) { rejected = true; }
            REQUIRE(rejected);
            REQUIRE(phase == flexedge::node::v2::APPLY_PHASE_ACTIVATE);
            REQUIRE(activationRuntime.appliedNodeSpecRevision() == 3);
            REQUIRE(goodStore.load().active.node_spec().content().revision() == 3);
            const auto rolledBackReports = plane.originHealthReports();
            REQUIRE(rolledBackReports.size() == 1);
            REQUIRE(rolledBackReports.front().websiteId == "website-1");
            REQUIRE(rolledBackReports.front().originId == "origin-1");
            REQUIRE(waitUntil([&] {
                asio::ip::tcp::acceptor probe(reservationContext);
                std::error_code error;
                probe.open(asio::ip::tcp::v4(), error);
                if (error) return false;
                probe.bind({asio::ip::address_v4::any(), candidatePort}, error);
                return !error;
            }, std::chrono::seconds(2)));
            control.loop(0).start(activateTestConfig(goodStore, plane, candidate, phase)).get();
            REQUIRE(activationRuntime.appliedNodeSpecRevision() == 4);
            REQUIRE(goodStore.load().active.node_spec().content().revision() == 4);
            const auto activatedReports = plane.originHealthReports();
            REQUIRE(activatedReports.size() == 1);
            REQUIRE(activatedReports.front().websiteId == "website-1");
            REQUIRE(activatedReports.front().originId == "origin-replacement");
            REQUIRE(request(candidatePort, "unknown.example.invalid").starts_with(
                "HTTP/1.1 421 Misdirected Request\r\n"));
            candidate.setGeneration(3);
            candidate.finalize();
            bool staleRejected = false;
            try {
                control.loop(0).start(activateTestConfig(goodStore, plane, candidate, phase)).get();
            } catch (const std::exception&) { staleRejected = true; }
            REQUIRE(staleRejected);
            REQUIRE(phase == flexedge::node::v2::APPLY_PHASE_VALIDATE);
            REQUIRE(activationRuntime.appliedNodeSpecRevision() == 4);
            REQUIRE(goodStore.load().active.node_spec().content().revision() == 4);
            REQUIRE(request(candidatePort, "unknown.example.invalid").starts_with(
                "HTTP/1.1 421 Misdirected Request\r\n"));
            shutdown.stopAndJoin();
        }

}
