#include <hiredis/hiredis.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/app.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "service/features/background/task_context.h"
#include "service/features/background/work_available.h"
#include "service/features/live_resource/notification_reader.h"
#include "service/features/log_ingest/queue.h"
#include "support/redis_test.h"

namespace {

using namespace std::chrono_literals;
namespace available = service::background::work_available;
namespace live = service::live_resource;
namespace logs = service::log_ingest;
using service::background::task_context;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

// Inspection and fault injection use one isolated test-owned connection.
// Production code stays on Ruvia's public pooled Redis API.
class redis_inspector final {
public:
    using reply_type = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
    explicit redis_inspector(const ruvia::redis_config& config)
        : connection_(redisConnectWithTimeout(config.host_.c_str(), config.port_, timeval{2, 0}), &redisFree) {
        require(connection_ && connection_->err == 0, "could not connect isolated Redis inspector");
        require(redisSetTimeout(connection_.get(), timeval{2, 0}) == REDIS_OK, "could not bound inspection timeout");
        if (!config.password_.empty()) {
            if (config.username_.empty()) {
                (void)request({"AUTH", config.password_});
            } else {
                (void)request({"AUTH", config.username_, config.password_});
            }
        }
        const auto database = std::to_string(config.database_);
        (void)request({"SELECT", database});
    }

    reply_type request(std::initializer_list<std::string_view> arguments) {
        std::vector<const char*> values;
        std::vector<std::size_t> lengths;
        for (const auto argument : arguments) {
            values.push_back(argument.data());
            lengths.push_back(argument.size());
        }
        reply_type result(static_cast<redisReply*>(redisCommandArgv(connection_.get(), static_cast<int>(values.size()),
                              values.data(), lengths.data())),
            &freeReplyObject);
        require(result && result->type != REDIS_REPLY_ERROR, "isolated Redis inspection failed");
        return result;
    }

    std::vector<std::string> blocked_readers() {
        const auto result = request({"CLIENT", "LIST"});
        require(result->type == REDIS_REPLY_STRING, "CLIENT LIST must return a string");
        std::string_view remaining(result->str, result->len);
        std::vector<std::string> ids;
        while (!remaining.empty()) {
            const auto newline = remaining.find('\n');
            auto line = remaining.substr(0, newline);
            std::string id;
            bool blocked{}, xread{};
            while (!line.empty()) {
                const auto space = line.find(' ');
                const auto field = line.substr(0, space);
                if (field.starts_with("id=")) {
                    id = field.substr(3);
                }
                if (field.starts_with("flags=")) {
                    blocked = field.substr(6).find('b') != std::string_view::npos;
                }
                if (field == "cmd=xread") {
                    xread = true;
                }
                if (space == std::string_view::npos) {
                    break;
                }
                line.remove_prefix(space + 1);
            }
            if (blocked) {
                require(xread, "only the combined notification reader may block");
                ids.push_back(std::move(id));
            }
            if (newline == std::string_view::npos) {
                break;
            }
            remaining.remove_prefix(newline + 1);
        }
        return ids;
    }

private:
    std::unique_ptr<redisContext, decltype(&redisFree)> connection_;
};

struct observations final {
    std::atomic_uint started_{};
    std::array<std::atomic_uint, available::types.size()> awakened_{};
    std::array<std::atomic_uint, available::types.size()> recovered_{};
    std::atomic_uint log_claims_{};
    std::atomic_uint joined_{};
};

ruvia::task<void> receive_hints(task_context& context, std::size_t index, observations& observed) {
    auto& signal = available::get_signals(context).for_type(available::types[index]);
    co_await signal.wait(10s, context.get_stop_token());
    if (context.get_stop_token().stop_requested()) {
        co_return;
    }
    observed.started_.fetch_add(1, std::memory_order_release);
    co_await signal.wait(10s, context.get_stop_token());
    if (context.get_stop_token().stop_requested()) {
        co_return;
    }
    if (available::types[index] == "log-ingest") {
        auto entries = co_await logs::queue::read(context.redis(), context.lease_owner());
        for (const auto& entry : entries) {
            require(entry.valid_ && entry.payload_ == "isolated-log-delivery", "real log queue retains its payload");
            co_await logs::queue::finish(context.redis(), context.lease_owner(), entry);
            observed.log_claims_.fetch_add(1, std::memory_order_release);
        }
    }
    observed.awakened_[index].fetch_add(1, std::memory_order_release);
    co_await signal.wait(10s, context.get_stop_token());
    if (!context.get_stop_token().stop_requested()) {
        observed.recovered_[index].fetch_add(1, std::memory_order_release);
    }
}

ruvia::task<void> run_suite(ruvia::web_worker_context& web, observations& observed) {
    task_context context(web);
    (void)available::get_signals(context);
    co_await logs::queue::initialize(context.redis());
    ruvia::task_scope tasks(web.worker(), {.resource_ = web.pool()});
    tasks.spawn(live::read_notifications(context));
    for (std::size_t index = 0; index < available::types.size(); ++index) {
        tasks.spawn(receive_hints(context, index, observed));
    }
    co_await tasks.join();
    observed.joined_.fetch_add(1, std::memory_order_release);
}

template <typename predicate_type>
void await_condition(predicate_type predicate, const char* failure) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    require(predicate(), failure);
}

std::uint16_t available_port() {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor(io, {asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

}  // namespace

int main() {
    try {
        auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        config->blocking_pool_size_per_worker_ = 1;
        redis_inspector inspector(*config);
        (void)inspector.request({"DEL", available::stream_key, available::generation_key, logs::queue::stream_key,
            logs::queue::retained_bytes_key, logs::queue::retry_schedule_key, logs::queue::retry_attempts_key,
            logs::queue::statistics_key, live::notifications::stream_key, logs::notifications::stream_key,
            service::node_channel::completion_stream_key});
        // A malformed shared hint key must fail before enqueue or ACK; Redis
        // Lua errors cannot undo mutations that preceded the error.
        (void)inspector.request({"SET", available::stream_key, "invalid-type"});
        bool rejected{};
        try {
            (void)inspector.request({"EVAL", logs::queue::push_script, "3", logs::queue::stream_key,
                logs::queue::retained_bytes_key, available::stream_key, "fixture", "100", "1000"});
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected && inspector.request({"XLEN", logs::queue::stream_key})->integer == 0,
            "failed hint validation must not leave an unannounced log payload");
        (void)inspector.request({"XGROUP", "CREATE", logs::queue::stream_key, logs::queue::consumer_group, "0", "MKSTREAM"});
        const auto fixture = inspector.request({"XADD", logs::queue::stream_key, "*", "payload", "fixture"});
        const std::string fixture_id(fixture->str, fixture->len);
        (void)inspector.request({"SET", logs::queue::retained_bytes_key, "7"});
        (void)inspector.request({"XREADGROUP", "GROUP", logs::queue::consumer_group, "fixture-owner", "STREAMS", logs::queue::stream_key, ">"});
        rejected = false;
        try {
            (void)inspector.request({"EVAL", logs::queue::finish_script, "6", logs::queue::stream_key,
                logs::queue::retained_bytes_key, logs::queue::retry_schedule_key, logs::queue::retry_attempts_key,
                logs::queue::statistics_key, available::stream_key, logs::queue::consumer_group, fixture_id, "fixture-owner"});
        } catch (const std::exception&) {
            rejected = true;
        }
        const auto pending = inspector.request({"XPENDING", logs::queue::stream_key, logs::queue::consumer_group});
        require(rejected && pending->type == REDIS_REPLY_ARRAY && pending->element[0]->integer == 1 &&
                    inspector.request({"XLEN", logs::queue::stream_key})->integer == 1,
            "failed completion hint validation must retain the owned pending payload");
        (void)inspector.request({"DEL", available::stream_key, logs::queue::stream_key, logs::queue::retained_bytes_key});
        auto& app = ruvia::app();
        app.redis({.config_ = *config});
        app.server({.worker_count_ = 2, .drain_timeout_ = 0ms});
        app.listen({.address_ = "127.0.0.1", .http_ = available_port()});
        app.use_worker_state<available::signals>();
        app.use_worker_state<live::hub>();
        app.use_worker_state<logs::fanout::hub>();
        app.use_worker_state<service::node_dispatch::fanout::hub>();
        app.use_worker_state<service::node_channel::report_waits>();
        observations observed;
        app.on_start([&] {
            const auto workers = app.workers();
            require(workers.size() == 2, "test requires two identical Web Workers");
            for (const auto& worker : workers) {
                require(worker.post([&](ruvia::web_worker_context& context) -> ruvia::task<void> {
                                  co_await run_suite(context, observed);
                              })
                            .accepted(),
                    "Web Worker suite was rejected");
            }
        });
        std::exception_ptr failure;
        std::jthread producer([&](std::stop_token stop) {
            try {
                await_condition([&] { return stop.stop_requested() || observed.started_.load(std::memory_order_acquire) == 14; },
                    "all seven task types must register and receive startup catchup on both workers");
                if (stop.stop_requested()) {
                    return;
                }
                await_condition([&] { return inspector.blocked_readers().size() == 2; },
                    "two Web Workers must use exactly two blocking connections");
                for (std::size_t index = 0; index < available::types.size(); ++index) {
                    const auto type = available::types[index];
                    if (type == "log-ingest") {
                        const auto maximum_entries = std::to_string(logs::queue::max_entries);
                        const auto maximum_bytes = std::to_string(logs::queue::max_retained_bytes);
                        const auto pushed = inspector.request({"EVAL", logs::queue::push_script, "3", logs::queue::stream_key,
                            logs::queue::retained_bytes_key, available::stream_key, "isolated-log-delivery", maximum_entries, maximum_bytes});
                        require(pushed->type == REDIS_REPLY_STRING, "production log enqueue Lua must append payload and hint atomically");
                    } else if (type != "outbox") {
                        (void)inspector.request({"XADD", available::stream_key, "*", "type", type});
                    }
                    await_condition([&] { return observed.awakened_[index].load(std::memory_order_acquire) == 2; },
                        "each broadcast hint must wake that type on both independent workers");
                    for (std::size_t other = index + 1; other < available::types.size(); ++other) {
                        if (type == "log-ingest" && available::types[other] == "outbox") {
                            continue;
                        }
                        require(observed.awakened_[other].load(std::memory_order_acquire) == 0,
                            "a hint must not wake unrelated task types");
                    }
                }
                require(observed.log_claims_.load(std::memory_order_acquire) == 1,
                    "both workers wake but the real log consumer group claims the payload once");
                const auto retained = inspector.request({"XLEN", available::stream_key});
                require(retained->integer == 7, "broadcast readers must not delete hints needed by another worker");
                const auto groups = inspector.request({"XINFO", "GROUPS", available::stream_key});
                require(groups->type == REDIS_REPLY_ARRAY && groups->elements == 0, "broadcast hints have no consumer group or pending metadata");
                await_condition([&] { return inspector.blocked_readers().size() == 2; }, "combined readers must return to BLOCK");
                for (const auto& id : inspector.blocked_readers()) {
                    (void)inspector.request({"CLIENT", "KILL", "ID", id});
                }
                for (std::size_t index = 0; index < available::types.size(); ++index) {
                    await_condition([&] { return observed.recovered_[index].load(std::memory_order_acquire) == 2; },
                        "connection recovery must wake every local task without another producer event");
                }
                await_condition([&] { return inspector.blocked_readers().size() == 2; }, "recovered readers must still use one connection each");
                std::cout << "Redis CLIENT LIST: two real Web Workers, seven waiting task types each, exactly two blocked XREAD connections\n";
            } catch (...) {
                failure = std::current_exception();
            }
            app.stop();
        });
        app.run();
        producer.request_stop();
        producer.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        require(observed.joined_.load(std::memory_order_acquire) == 2, "shutdown must join both readers and all local task waits");
        require(inspector.blocked_readers().empty(), "shutdown must leave no blocked reader connection");
        std::cout << "Task wakeups: type isolation, broadcast, independent log claims, reconnect catchup and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
