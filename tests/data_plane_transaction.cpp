#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"

#define FLEXEDGE_REQUIRE(condition)                                      \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;
using flexedge::testing::TestConfig;
using namespace std::chrono_literals;

struct temporary_state final {
    temporary_state()
        : path_(std::filesystem::temp_directory_path() /
                ("flexedge-activation-fault-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}
    ~temporary_state() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path path_;
};

// Only test instrumentation blocks the worker thread. The callback owns its barrier and
// has a bounded release time, even if an assertion fails on the driving thread.
class worker_barrier final {
    struct state final {
        std::mutex mutex_;
        std::condition_variable changed_;
        bool entered_{};
        bool released_{};
    };

public:
    worker_barrier(ruvia::event_loop loop, std::chrono::steady_clock::time_point release_at)
        : state_(std::make_shared<state>()) {
        FLEXEDGE_REQUIRE(loop.post([state = state_, release_at] {
                                 std::unique_lock lock(state->mutex_);
                                 state->entered_ = true;
                                 state->changed_.notify_all();
                                 state->changed_.wait_until(lock, release_at, [&] { return state->released_; });
                             })
                .accepted());
        std::unique_lock lock(state_->mutex_);
        FLEXEDGE_REQUIRE(state_->changed_.wait_for(lock, 2s, [&] { return state_->entered_; }));
    }
    ~worker_barrier() {
        release();
    }
    void release() {
        std::lock_guard lock(state_->mutex_);
        state_->released_ = true;
        state_->changed_.notify_all();
    }

private:
    std::shared_ptr<state> state_;
};

struct activation_gate final {
    explicit activation_gate(ruvia::event_loop loop)
        : channel_(ruvia::make_channel<bool>(loop.handle(), {.capacity_ = 1})) {}
    void wait_prepared() {
        FLEXEDGE_REQUIRE(flexedge::testing::waitUntil([&] { return ready_.load(); }, 2s));
    }
    void proceed() {
        FLEXEDGE_REQUIRE(channel_.first.send(true).accepted());
    }
    std::pair<ruvia::channel_sender<bool>, ruvia::channel_receiver<bool>> channel_;
    std::chrono::steady_clock::time_point deadline_{};
    std::atomic<bool> ready_{};
};

ruvia::task<void> activate_test_config(state_store& store, data_plane& plane, TestConfig config,
    std::chrono::steady_clock::duration timeout = 10s, activation_gate* gate = nullptr) {
    config.finalize();
    store.stage(config.active, config.objects);
    if (gate) {
        gate->deadline_ = std::chrono::steady_clock::now() + timeout;
    }
    auto reload = co_await plane.prepare(std::make_shared<const compiled_config>(config.active, config.objects), timeout);
    if (gate) {
        gate->ready_ = true;
        FLEXEDGE_REQUIRE((co_await gate->channel_.second.receive_for(5s)).has_value());
    }
    co_await activate_staged_config(store, plane, std::move(reload));
}

class fixture final {
public:
    fixture()
        : logs_(ruvia::make_channel<owned_log_batch>(control_.loop(0).handle(), {.capacity_ = 32})),
          plane_(workers_, control_.loop(0), runtime_, logs_.first),
          store_(state_.path_, "synthetic-activation-fault-secret"),
          shutdown_(workers_, control_) {
        workers_.start();
        control_.start();
        config_ = flexedge::testing::snapshot(false);
        config_.node_spec.set_enabled(false);
        control_.loop(0).start(activate_test_config(store_, plane_, config_)).get();
    }
    TestConfig candidate() const {
        auto next = config_;
        next.setGeneration(4);
        next.mutableWebsite()->mutable_origins(0)->set_id("replacement-origin");
        return next;
    }
    void require_old_state() {
        FLEXEDGE_REQUIRE(runtime_.applied_node_spec_revision() == 3);
        FLEXEDGE_REQUIRE(store_.load().contents_.node_spec_.revision() == 3);
        const auto report = control_.loop(0).start(plane_.report({})).get();
        // A load report is available only when every worker reports this same config.
        FLEXEDGE_REQUIRE(report.load_.has_value());
        const auto origins = report.origin_health_->reports();
        FLEXEDGE_REQUIRE(origins.size() == 1 && origins.front().origin_id_ == "origin-1");
    }
    void flush_control() {
        control_.loop(0).start([]() -> ruvia::task<void> { co_return; }()).get();
    }

    temporary_state state_;
    ruvia::event_loop_pool workers_{{.loop_count_ = 2}};
    ruvia::event_loop_pool control_{{.loop_count_ = 1}};
    runtime_state runtime_;
    std::pair<ruvia::channel_sender<owned_log_batch>, ruvia::channel_receiver<owned_log_batch>> logs_;
    data_plane plane_;
    state_store store_;
    TestConfig config_{};
    loop_shutdown shutdown_;
};

void verify_activation_timeout() {
    fixture test;
    activation_gate gate(test.control_.loop(0));
    auto applying = test.control_.loop(0).start(activate_test_config(test.store_, test.plane_, test.candidate(), 1s, &gate));
    gate.wait_prepared();
    worker_barrier blocked(test.workers_.loop(1), gate.deadline_ + 200ms);
    gate.proceed();
    bool timed_out{};
    try {
        applying.get();
    } catch (const data_plane_unavailable&) {
        throw;
    } catch (const std::runtime_error& error) {
        timed_out = std::string_view(error.what()).find("timed out") != std::string_view::npos;
    }
    FLEXEDGE_REQUIRE(timed_out);
    test.require_old_state();
    // The delayed activate and its late reply cannot resurrect the aborted transaction.
    test.control_.loop(0).start(activate_test_config(test.store_, test.plane_, test.candidate())).get();
    FLEXEDGE_REQUIRE(test.runtime_.applied_node_spec_revision() == 4);
    FLEXEDGE_REQUIRE(test.store_.load().contents_.node_spec_.revision() == 4);
    FLEXEDGE_REQUIRE(test.control_.loop(0).start(test.plane_.report({})).get().load_.has_value());
}

void verify_shutdown_during_activation() {
    fixture test;
    activation_gate gate(test.control_.loop(0));
    auto applying = test.control_.loop(0).start(activate_test_config(test.store_, test.plane_, test.candidate(), 2s, &gate));
    gate.wait_prepared();
    worker_barrier blocked(test.workers_.loop(1), gate.deadline_ + 1s);
    gate.proceed();
    test.flush_control();
    test.plane_.request_stop_accepting();
    blocked.release();
    bool aborted{};
    try {
        applying.get();
    } catch (const data_plane_unavailable&) {
        throw;
    } catch (const std::runtime_error&) {
        aborted = true;
    }
    FLEXEDGE_REQUIRE(aborted);
    test.require_old_state();
}

void verify_worker_exit() {
    fixture test;
    activation_gate gate(test.control_.loop(0));
    auto applying = test.control_.loop(0).start(activate_test_config(test.store_, test.plane_, test.candidate(), 2s, &gate));
    gate.wait_prepared();
    worker_barrier blocked(test.workers_.loop(1), gate.deadline_ + 1s);
    gate.proceed();
    test.flush_control();
    // A runtime failure stops the Ruvia data pool, while the control loop remains alive
    // to restore the durable state and observe every worker's cleanup acknowledgement.
    test.workers_.stop();
    blocked.release();
    bool unavailable{};
    try {
        applying.get();
    } catch (const data_plane_unavailable&) {
        unavailable = true;
    }
    FLEXEDGE_REQUIRE(unavailable);
    FLEXEDGE_REQUIRE(test.runtime_.applied_node_spec_revision() == 3);
    FLEXEDGE_REQUIRE(test.store_.load().contents_.node_spec_.revision() == 3);
    test.shutdown_.stop_and_join();
}

void verify_rollback_timeout() {
    fixture test;
    activation_gate gate(test.control_.loop(0));
    auto applying = test.control_.loop(0).start(activate_test_config(test.store_, test.plane_, test.candidate(), 500ms, &gate));
    gate.wait_prepared();
    worker_barrier blocked(test.workers_.loop(1), gate.deadline_ + 2s);
    gate.proceed();
    bool unavailable{};
    try {
        applying.get();
    } catch (const data_plane_unavailable&) {
        unavailable = true;
    }
    FLEXEDGE_REQUIRE(unavailable);
    FLEXEDGE_REQUIRE(test.runtime_.applied_node_spec_revision() == 3);
    FLEXEDGE_REQUIRE(test.store_.load().contents_.node_spec_.revision() == 3);
    blocked.release();
    // Fatal unavailability is followed by assembly shutdown, never a control reconnect.
    test.shutdown_.stop_and_join();
}
}  // namespace

int main() {
    const char* scenario = "activation timeout";
    try {
        verify_activation_timeout();
        scenario = "shutdown during activation";
        verify_shutdown_during_activation();
        scenario = "worker exit";
        verify_worker_exit();
        scenario = "rollback timeout";
        verify_rollback_timeout();
    } catch (const std::exception& error) {
        std::cerr << scenario << ": " << error.what() << '\n';
        return 1;
    }
}
