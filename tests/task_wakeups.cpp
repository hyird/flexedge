#include <hiredis/hiredis.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/app.h>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
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
        : database_(std::to_string(config.database_)),
          connection_(redisConnectWithTimeout(config.host_.c_str(), config.port_, timeval{2, 0}), &redisFree) {
        require(connection_ && connection_->err == 0, "could not connect isolated Redis inspector");
        require(redisSetTimeout(connection_.get(), timeval{2, 0}) == REDIS_OK, "could not bound inspection timeout");
        if (!config.password_.empty()) {
            if (config.username_.empty()) {
                (void)request({"AUTH", config.password_});
            } else {
                (void)request({"AUTH", config.username_, config.password_});
            }
        }
        (void)request({"SELECT", database_});
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
            bool blocked{}, xread{}, same_database{};
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
                if (field.starts_with("db=")) {
                    same_database = field.substr(3) == database_;
                }
                if (space == std::string_view::npos) {
                    break;
                }
                line.remove_prefix(space + 1);
            }
            // CLIENT LIST spans every logical database on the server. A
            // candidate service may legitimately block in another database.
            if (blocked && same_database) {
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
    const std::string database_;
    std::unique_ptr<redisContext, decltype(&redisFree)> connection_;
};

struct observations final {
    std::atomic_uint started_{};
    // Wakeups each task type received on each of the two workers.
    std::array<std::array<std::atomic_uint, available::types.size()>, 2> awakened_{};
    std::atomic_uint log_claims_{};
    std::atomic_uint joined_{};
};

ruvia::task<void> receive_hints(task_context& context, std::size_t worker, std::size_t index, observations& observed) {
    const auto own = available::subscribe(context, available::types[index]);
    observed.started_.fetch_add(1, std::memory_order_release);
    const auto woken = co_await own.receive_for(30s, context.get_stop_token());
    if (context.get_stop_token().stop_requested() || !woken.has_value()) {
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
    observed.awakened_[worker][index].fetch_add(1, std::memory_order_release);
    (void)co_await own.receive_for(30s, context.get_stop_token());
}

ruvia::task<void> run_suite(ruvia::web_worker_context& web, std::size_t worker, observations& observed) {
    task_context context(web);
    co_await logs::queue::initialize(context.redis());
    ruvia::task_scope tasks(web.worker(), {.resource_ = web.pool()});
    tasks.spawn(live::run_notification_reader<live::resource_subscriptions>(context));
    for (std::size_t index = 0; index < available::types.size(); ++index) {
        tasks.spawn(receive_hints(context, worker, index, observed));
    }
    co_await tasks.join();
    observed.joined_.fetch_add(1, std::memory_order_release);
}

// One worker commits work of `type` (a log delivery is really enqueued) and
// wakes only its own claim loop for it.
ruvia::task<void> commit_own_work(ruvia::web_worker_context& web, std::string_view type) {
    task_context context(web);
    if (type == "log-ingest") {
        require(co_await logs::queue::push(context.redis(), "isolated-log-delivery"), "the log queue accepts the delivery");
    }
    const std::array work{type};
    available::wake_own_claims(context, work);
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

// The former shared broadcast stream; no producer may write it any more.
constexpr std::string_view retired_broadcast_key{"flexedge:work-available:v1"};

}  // namespace

int main() {
    try {
        auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        config->blocking_pool_size_per_worker_ = 1;
        redis_inspector inspector(*config);
        (void)inspector.request({"DEL", retired_broadcast_key, available::generation_key, logs::queue::stream_key,
            logs::queue::retained_bytes_key, logs::queue::retry_schedule_key, logs::queue::retry_attempts_key,
            logs::queue::statistics_key, live::notifications::stream_key});
        auto& app = ruvia::app();
        app.redis({.config_ = *config});
        app.server({.worker_count_ = 2, .drain_timeout_ = 0ms});
        app.listen({.address_ = "127.0.0.1", .http_ = available_port()});
        app.use_worker_state<available::claim_signals>();
        app.use_worker_state<live::hub>();
        observations observed;
        std::optional<decltype(app.workers())> workers;
        app.on_start([&] {
            workers.emplace(app.workers());
            require(workers->size() == 2, "test requires two identical Web Workers");
            for (std::size_t index = 0; index < workers->size(); ++index) {
                require((*workers)[index]
                            .post([&observed, index](ruvia::web_worker_context& context) {
                                return run_suite(context, index, observed);
                            })
                            .accepted(),
                    "Web Worker suite was rejected");
            }
        });
        std::exception_ptr failure;
        std::jthread producer([&](std::stop_token stop) {
            try {
                await_condition([&] { return stop.stop_requested() || observed.started_.load(std::memory_order_acquire) == 14; },
                    "all seven task types must subscribe on both workers");
                if (stop.stop_requested()) {
                    return;
                }
                await_condition([&] { return inspector.blocked_readers().size() == 2; },
                    "two Web Workers must use exactly two blocking connections");
                for (std::size_t index = 0; index < available::types.size(); ++index) {
                    const auto type = available::types[index];
                    require((*workers)[0]
                                .post([type](ruvia::web_worker_context& context) { return commit_own_work(context, type); })
                                .accepted(),
                        "the committing worker accepts its job");
                    await_condition([&] { return observed.awakened_[0][index].load(std::memory_order_acquire) == 1; },
                        "the committing worker wakes its own claim loop of that type");
                    for (std::size_t other = index + 1; other < available::types.size(); ++other) {
                        require(observed.awakened_[0][other].load(std::memory_order_acquire) == 0,
                            "a wakeup must not wake unrelated task types");
                    }
                }
                std::this_thread::sleep_for(200ms);
                for (std::size_t index = 0; index < available::types.size(); ++index) {
                    require(observed.awakened_[1][index].load(std::memory_order_acquire) == 0,
                        "committing work never wakes another worker; its bounded scan finds the work instead");
                }
                require(observed.log_claims_.load(std::memory_order_acquire) == 1,
                    "the committing worker's log consumer claims the payload");
                require(inspector.request({"EXISTS", retired_broadcast_key})->integer == 0,
                    "no producer appends a broadcast wakeup hint");
                await_condition([&] { return inspector.blocked_readers().size() == 2; }, "combined readers must return to BLOCK");
                for (const auto& id : inspector.blocked_readers()) {
                    (void)inspector.request({"CLIENT", "KILL", "ID", id});
                }
                await_condition([&] { return inspector.blocked_readers().size() == 2; }, "recovered readers must still use one connection each");
                std::cout << "Redis CLIENT LIST: two real Web Workers, seven waiting task types each, exactly two blocked XREAD connections\n";
            } catch (...) {
                failure = std::current_exception();
            }
            app.stop();
        });
        std::exception_ptr run_failure;
        try {
            app.run();
        } catch (...) {
            run_failure = std::current_exception();
        }
        producer.request_stop();
        producer.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        if (run_failure) {
            std::rethrow_exception(run_failure);
        }
        require(observed.joined_.load(std::memory_order_acquire) == 2, "shutdown must join both readers and all local task waits");
        require(inspector.blocked_readers().empty(), "shutdown must leave no blocked reader connection");
        std::cout << "Task wakeups: committer-only wakeups, type isolation, single log claim, reader recovery and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
