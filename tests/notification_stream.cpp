#include <hiredis/hiredis.h>
#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/redis/redis_client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/post.hpp>

#include "service/features/live_resource/notification_reader.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_dispatch/fanout.h"
#include "support/redis_test.h"

namespace {

namespace live = service::live_resource;
namespace logs = service::log_ingest;
namespace available = service::background::work_available;
using clock_type = std::chrono::steady_clock;
// The production handler list of every Web Worker (server.cpp).
using reader = live::notification_reader<live::resource_subscriptions, service::node_dispatch::fanout::desired_state_waits,
    logs::fanout::log_tails, logs::fanout::log_tail_recovery, service::node_channel::report_receipts>;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("combined notification reader: " + std::string(message));
    }
}

struct reader_context final {
    ruvia::redis_client& redis_;
    ruvia::stop_source stop_;
    live::hub resources_;
    logs::fanout::hub logs_;
    service::node_dispatch::fanout::hub dispatch_;
    service::node_channel::report_waits report_waits_;
    std::optional<service::node_channel::report_waits::subscription> report_subscription_;
    std::optional<live::hub::subscription> resource_subscription_;
    std::optional<logs::fanout::hub::subscription> log_subscription_;
    std::optional<live::hub::subscription> other_tenant_resource_;
    std::optional<logs::fanout::hub::subscription> other_tenant_log_;

    explicit reader_context(ruvia::redis_client& redis)
        : redis_(redis) {}
    ruvia::redis_handle redis() const {
        return redis_.handle();
    }
    const ruvia::worker_handle& worker() const {
        return redis_.worker();
    }
    ruvia::stop_token get_stop_token() const {
        return stop_.token();
    }

    template <typename state_type>
    state_type& worker_state() {
        if constexpr (std::is_same_v<state_type, live::hub>) {
            return resources_;
        } else if constexpr (std::is_same_v<state_type, logs::fanout::hub>) {
            return logs_;
        } else if constexpr (std::is_same_v<state_type, service::node_channel::report_waits>) {
            return report_waits_;
        } else {
            static_assert(std::is_same_v<state_type, service::node_dispatch::fanout::hub>);
            return dispatch_;
        }
    }
};

// Suspend one actual Worker while Redis replies to its already-blocked XREAD.
// Producers on another loop can then trim that returned batch before the
// receiver resumes, without disconnecting or restarting its reader.
class paused_worker final {
public:
    explicit paused_worker(const ruvia::event_loop& loop)
        : release_(std::make_shared<std::promise<void>>()) {
        auto entered = std::make_shared<std::promise<void>>();
        auto entered_future = entered->get_future();
        auto released = release_->get_future().share();
        asio::post(loop.executor(), [entered, released] {
            entered->set_value();
            released.wait();
        });
        entered_future.get();
    }
    ~paused_worker() {
        resume();
    }
    paused_worker(const paused_worker&) = delete;
    paused_worker& operator=(const paused_worker&) = delete;
    void resume() {
        if (release_) {
            release_->set_value();
            release_.reset();
        }
    }

private:
    std::shared_ptr<std::promise<void>> release_;
};

ruvia::task<void> subscribe(reader_context& context) {
    context.resource_subscription_.emplace(context.resources_.subscribe(context.worker(), "tenant-a", live::resource::websites, "website-a"));
    context.log_subscription_.emplace(context.logs_.subscribe(context.worker(), logs::notifications::log_resource_type::node, "tenant-a", "node-a"));
    context.other_tenant_resource_.emplace(context.resources_.subscribe(context.worker(), "tenant-b", live::resource::websites, "website-a"));
    context.other_tenant_log_.emplace(context.logs_.subscribe(context.worker(), logs::notifications::log_resource_type::node, "tenant-b", "node-a"));
    context.report_subscription_.emplace(context.report_waits_.subscribe(context.worker(), "123-1"));
    co_return;
}

ruvia::task<void> initial_log_snapshot(reader_context& context) {
    require((co_await context.log_subscription_->receive_for(std::chrono::seconds(2), {})).has_value(), "the real reader initializes its own log tails");
    require((co_await context.other_tenant_log_->receive_for(std::chrono::seconds(2), {})).has_value(), "initial readiness covers every local log tail");
    require((co_await context.report_subscription_->receive_for(std::chrono::seconds(1), {})).has_value(),
        "startup rechecks receipts that raced the completion cursor snapshot");
}

ruvia::task<void> receive_receipt(reader_context& context) {
    require((co_await context.report_subscription_->receive_for(std::chrono::seconds(2), {})).has_value(),
        "completion ids wake only this worker's active matching receipt subscription");
}

ruvia::task<void> quiet(reader_context& context) {
    require(!(co_await context.resource_subscription_->receive_for(std::chrono::milliseconds(40), {})).has_value(), "idle resource subscriptions receive no duplicate snapshots");
    require(!(co_await context.log_subscription_->receive_for(std::chrono::milliseconds(40), {})).has_value(), "idle log subscriptions receive no duplicate signals");
    require(!(co_await context.other_tenant_resource_->receive_for(std::chrono::milliseconds(40), {})).has_value(), "business events do not cross resource tenant boundaries");
    require(!(co_await context.other_tenant_log_->receive_for(std::chrono::milliseconds(40), {})).has_value(), "business events do not cross log tenant boundaries");
}

ruvia::task<void> receive_business_change(reader_context& context) {
    require((co_await context.resource_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(), "the resource stream reaches this worker's resource hub");
    const auto change = context.resource_subscription_->drain();
    require(change.snapshot_ && !change.recovery_, "a committed change has the normal snapshot semantics");
    require((co_await context.log_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(), "the log stream reaches this worker's log hub");
}

ruvia::task<void> receive_recovery(reader_context& context) {
    require((co_await context.resource_subscription_->receive_for(std::chrono::seconds(22), {})).has_value(), "a reconnected idle reader recovers resources without a new business event");
    const auto recovered = context.resource_subscription_->drain();
    require(recovered.snapshot_ && recovered.recovery_, "resource recovery forces a complete snapshot");
    require((co_await context.log_subscription_->receive_for(std::chrono::seconds(2), {})).has_value(), "the same reconnection also wakes local log tails");
    require((co_await context.other_tenant_resource_->receive_for(std::chrono::seconds(2), {})).has_value(), "connection recovery covers all local resource tenants");
    (void)context.other_tenant_resource_->drain();
    require((co_await context.other_tenant_log_->receive_for(std::chrono::seconds(2), {})).has_value(), "connection recovery covers all local log tenants");
    require((co_await context.report_subscription_->receive_for(std::chrono::seconds(1), {})).has_value(),
        "recovery rechecks active connection receipts");
}

ruvia::task<void> receive_tenant_recovery(reader_context& context) {
    require((co_await context.resource_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(), "tenant recovery reaches its resource subscription");
    require(context.resource_subscription_->drain().recovery_, "tenant recovery forces a resource snapshot");
    require((co_await context.log_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(), "tenant recovery also refills its log tails");
}

ruvia::task<void> receive_resource_gap(reader_context& context) {
    require((co_await context.resource_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(),
        "a trimmed unread resource event forces a recovery snapshot");
    require(context.resource_subscription_->drain().recovery_, "trim recovery preserves SSE snapshot semantics");
    require((co_await context.other_tenant_resource_->receive_for(std::chrono::seconds(3), {})).has_value(),
        "unknown trimmed resource changes recover all local resource tenants");
    require(context.other_tenant_resource_->drain().recovery_, "the second resource tenant also recovers");
    require(!(co_await context.log_subscription_->receive_for(std::chrono::milliseconds(40), {})).has_value(),
        "a resource-only trim does not reset unrelated log tails");
    require(!(co_await context.other_tenant_log_->receive_for(std::chrono::milliseconds(40), {})).has_value(),
        "a resource-only trim leaves other log tenants quiet");
}

ruvia::task<void> receive_log_gap(reader_context& context) {
    require((co_await context.log_subscription_->receive_for(std::chrono::seconds(3), {})).has_value(),
        "a trimmed unread log event refills this worker's log tail");
    require((co_await context.other_tenant_log_->receive_for(std::chrono::seconds(3), {})).has_value(),
        "unknown trimmed log changes recover every local log tenant");
    require(!(co_await context.resource_subscription_->receive_for(std::chrono::milliseconds(40), {})).has_value(),
        "a log-only trim does not manufacture resource snapshots");
    require(!(co_await context.other_tenant_resource_->receive_for(std::chrono::milliseconds(40), {})).has_value(),
        "a log-only trim leaves other resource tenants quiet");
}

ruvia::task<void> receive_receipt_gap(reader_context& context) {
    require((co_await context.report_subscription_->receive_for(std::chrono::seconds(1), {})).has_value(),
        "a trimmed completion hint rechecks the original connection's receipt");
    require(!(co_await context.report_subscription_->receive_for(std::chrono::milliseconds(40), {})).has_value(),
        "one completion gap does not produce duplicate receipt signals");
}

struct redis_client_info final {
    std::string id_;
    std::string database_;
    std::string flags_;
    std::string command_;
};

// CLIENT is intentionally forbidden by Ruvia's pooled API. This isolated
// test owns one separate, bounded administrative connection for inspecting
// and disconnecting the exact reader client IDs observed below.
class redis_inspector final {
public:
    using reply_type = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    explicit redis_inspector(const ruvia::redis_config& config)
        : connection_(redisConnectWithTimeout(config.host_.c_str(), config.port_, timeval{2, 0}), &redisFree) {
        require(connection_ && connection_->err == 0, "the isolated inspection connection opens");
        require(redisSetTimeout(connection_.get(), timeval{2, 0}) == REDIS_OK, "inspection commands have a bounded timeout");
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
        reply_type reply(static_cast<redisReply*>(redisCommandArgv(connection_.get(), static_cast<int>(values.size()), values.data(), lengths.data())), &freeReplyObject);
        require(reply && reply->type != REDIS_REPLY_ERROR, "the isolated inspection command succeeds");
        return reply;
    }

private:
    std::unique_ptr<redisContext, decltype(&redisFree)> connection_;
};

std::vector<redis_client_info> parse_clients(std::string_view listing) {
    std::vector<redis_client_info> clients;
    while (!listing.empty()) {
        const auto newline = listing.find('\n');
        auto line = listing.substr(0, newline);
        redis_client_info client{};
        while (!line.empty()) {
            const auto space = line.find(' ');
            const auto field = line.substr(0, space);
            const auto equals = field.find('=');
            const auto key = field.substr(0, equals);
            const auto value = equals == std::string_view::npos ? std::string_view{} : field.substr(equals + 1);
            if (key == "id") {
                client.id_ = value;
            } else if (key == "db") {
                client.database_ = value;
            } else if (key == "flags") {
                client.flags_ = value;
            } else if (key == "cmd") {
                client.command_ = value;
            }
            if (space == std::string_view::npos) {
                break;
            }
            line.remove_prefix(space + 1);
        }
        if (!client.id_.empty()) {
            clients.push_back(std::move(client));
        }
        if (newline == std::string_view::npos) {
            break;
        }
        listing.remove_prefix(newline + 1);
    }
    return clients;
}

std::vector<redis_client_info> inspect_clients(redis_inspector& inspector) {
    const auto reply = inspector.request({"CLIENT", "LIST"});
    require(reply->type == REDIS_REPLY_STRING, "CLIENT LIST returns an actual client listing");
    return parse_clients(std::string_view(reply->str, reply->len));
}

ruvia::task<std::vector<std::string>> wait_blocked(ruvia::redis_client& admin, redis_inspector& inspector, std::uint32_t database, std::size_t count) {
    const auto expected_database = std::to_string(database);
    const auto deadline = clock_type::now() + std::chrono::seconds(5);
    do {
        const auto clients = inspect_clients(inspector);
        std::vector<std::string> blocked;
        for (const auto& client : clients) {
            if (client.database_ == expected_database && client.command_ == "xread" && client.flags_.find('b') != std::string::npos) {
                blocked.push_back(client.id_);
            }
        }
        if (blocked.size() == count) {
            std::cout << "Redis CLIENT LIST: blocked XREAD=" << count << "; ids=";
            for (const auto& id : blocked) {
                std::cout << id << ' ';
            }
            std::cout << '\n';
            co_return blocked;
        }
        (void)co_await ruvia::sleep_for(admin.worker(), std::chrono::milliseconds(25), {});
    } while (clock_type::now() < deadline);
    throw std::runtime_error("combined notification reader did not reach the expected blocked connection count");
}

ruvia::task<void> publish_fields(ruvia::redis_client& admin, std::string_view stream,
    const std::vector<std::pair<std::string, std::string>>& fields) {
    std::vector<std::string_view> arguments{"XADD", stream, "*"};
    for (const auto& [name, value] : fields) {
        arguments.push_back(name);
        arguments.push_back(value);
    }
    const auto result = co_await admin.handle().command(std::span<const std::string_view>(arguments));
    service::utils::require_redis_success(result, "could not publish a combined reader fixture");
}

ruvia::task<void> publish_change(ruvia::redis_client& admin) {
    const auto fields = live::notifications::encode(std::move(live::notifications::resource_notification("tenant-a").changed(live::resource::websites, "website-a")));
    co_await publish_fields(admin, live::notifications::stream_key, fields);
    const std::vector<std::pair<std::string, std::string>> log_fields{{"tenant_id", "tenant-a"}, {"resource_type", "node"}, {"resource_id", "node-a"}};
    co_await publish_fields(admin, logs::notifications::stream_key, log_fields);
}

ruvia::task<void> publish_receipt(ruvia::redis_client& admin) {
    const std::vector<std::pair<std::string, std::string>> fields{{"report_id", "123-1"}};
    co_await publish_fields(admin, service::node_channel::completion_stream_key, fields);
}

ruvia::task<void> reset_generation(ruvia::redis_client& admin) {
    (void)co_await admin.handle().del(available::generation_key);
    // This append returns the existing BLOCK normally. The next generation
    // check must notice lost metadata without relying on a command failure.
    const std::vector<std::pair<std::string, std::string>> fields{{"report_id", "unrelated-report"}};
    co_await publish_fields(admin, service::node_channel::completion_stream_key, fields);
}

ruvia::task<void> publish_tenant_recovery(ruvia::redis_client& admin) {
    live::notifications::resource_notification notification("tenant-a");
    notification.missed_until_ = live::unix_milliseconds();
    const auto fields = live::notifications::encode(notification);
    co_await publish_fields(admin, live::notifications::stream_key, fields);
}

ruvia::task<void> disconnect_readers(redis_inspector& inspector, std::vector<std::string> ids) {
    for (const auto& id : ids) {
        const auto result = inspector.request({"CLIENT", "KILL", "ID", id});
        require(result->type == REDIS_REPLY_INTEGER && result->integer == 1, "only the identified blocked test connection is disconnected");
    }
    co_return;
}

ruvia::task<void> clean(ruvia::redis_client& admin) {
    (void)co_await admin.handle().command("DEL", live::notifications::stream_key, logs::notifications::stream_key,
        available::generation_key, service::node_channel::completion_stream_key);
    for (const auto key : live::redis_snapshot_cache::keys) {
        (void)co_await admin.handle().command("DEL", key);
    }
}

ruvia::task<void> trim_notifications(ruvia::redis_client& admin, const std::vector<std::size_t>& indexes) {
    constexpr std::string_view script{R"lua(
for i,key in ipairs(KEYS) do
 local mode=tonumber(ARGV[i])
 for n=1,8 do
  local tenant='unrelated-tenant'
  if mode==0 then redis.call('XADD',key,'*','tenant_id',tenant,'changes',ARGV[#KEYS+1])
  elseif mode==1 then redis.call('XADD',key,'*','tenant_id',tenant,'resource_type','node','resource_id','node-a')
  else redis.call('XADD',key,'*','report_id','unrelated-report') end
 end
 redis.call('XTRIM',key,'MAXLEN',2)
end
return 1
)lua"};
    const auto fields = live::notifications::encode(live::notifications::resource_notification("tenant-a").changed(live::resource::websites, "website-a"));
    const auto change = std::find_if(fields.begin(), fields.end(), [](const auto& field) { return field.first == "changes"; });
    require(change != fields.end(), "resource trim fixture uses the real notification encoding");
    const auto generation = co_await admin.handle().command("GET", available::generation_key);
    std::vector<std::string_view> keys;
    std::vector<std::string> modes;
    for (const auto index : indexes) {
        // This command wakes the suspended readers' XREAD before the next
        // command removes that returned event from the retained prefix.
        if (index == 0) {
            co_await publish_fields(admin, reader::keys[index], fields);
        } else if (index == 1) {
            const std::vector<std::pair<std::string, std::string>> values{{"tenant_id", "tenant-a"}, {"resource_type", "node"}, {"resource_id", "node-a"}};
            co_await publish_fields(admin, reader::keys[index], values);
        } else {
            const std::vector<std::pair<std::string, std::string>> values{{"report_id", "123-1"}};
            co_await publish_fields(admin, reader::keys[index], values);
        }
        keys.push_back(reader::keys.at(index));
        modes.push_back(std::to_string(index));
    }
    std::vector<std::string_view> arguments;
    for (const auto& mode : modes) {
        arguments.push_back(mode);
    }
    arguments.push_back(change->second);
    service::utils::require_redis_success(co_await admin.handle().eval(script, keys, arguments), "could not trim notification fixtures");
    const auto unchanged = co_await admin.handle().command("GET", available::generation_key);
    require(generation.string() == unchanged.string(), "trimming does not rely on a generation reset");
}

ruvia::task<void> verify_atomic_batches(ruvia::redis_client& admin) {
    std::array<live::notification_cursor, reader::keys.size()> cursors;
    const auto generation = co_await available::generation(admin.handle());
    constexpr std::string_view append{R"lua(
for i=1,tonumber(ARGV[1]) do redis.call('XADD',KEYS[1],'*','fixture','value') end
if ARGV[2]~='' then redis.call('XTRIM',KEYS[1],'MAXLEN',ARGV[2]) end
return 1
)lua"};
    const std::array keys{reader::keys[0]};
    const std::array<std::string_view, 2> initial{"3", ""};
    (void)co_await admin.handle().eval(append, keys, initial);
    const auto heads = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, true);
    require(live::matching_notification_generation(heads, generation, reader::keys.size()), "head capture includes the same atomic generation");
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        live::accept_notification_cursor(cursors[index], heads.array()[1].array()[index]);
    }
    const std::array<std::string_view, 2> boundary{"2", "2"};
    (void)co_await admin.handle().eval(append, keys, boundary);
    const auto accepted = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, false);
    const auto first = accepted.array()[1].array()[0].array();
    require(first[4].integer() == 0 && first[1].array().size() == 2,
        "trimming only previously consumed entries does not trigger recovery");
    require(first[3].integer() == 5, "cursor and accepted ordinal describe the same atomic batch");
    const std::array<std::string_view, 2> overrun{"8", "2"};
    (void)co_await admin.handle().eval(append, keys, overrun);
    require(first[1].array().size() == 2, "a previously returned batch owns its bytes despite later Redis trimming");
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        live::accept_notification_cursor(cursors[index], accepted.array()[1].array()[index]);
    }
    const auto missed = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, false);
    const auto gap = missed.array()[1].array()[0].array();
    require(gap[4].integer() == 1 && gap[1].array().empty() && gap[3].integer() == 13,
        "trim after an accepted batch recovers exactly the missing suffix and advances to the current head");
    for (std::size_t index = 0; index < cursors.size(); ++index) {
        live::accept_notification_cursor(cursors[index], missed.array()[1].array()[index]);
    }
    const auto quiet = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, false);
    require(quiet.array()[1].array()[0].array()[4].integer() == 0,
        "an acknowledged gap cannot generate repeated recovery snapshots");

    std::array<std::string, 3> appended;
    for (auto& id : appended) {
        const auto entry = co_await admin.handle().command("XADD", keys[0], "*", "fixture", "value");
        id = std::string(entry.string());
    }
    (void)co_await admin.handle().command("XDEL", keys[0], appended[1]);
    const auto hole = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, false);
    require(hole.array()[1].array()[0].array()[4].integer() == 1,
        "an unread middle deletion is detected without a prefix overrun");
    auto unknown_counter = cursors;
    unknown_counter[0] = {.id_ = "0-0", .seen_entries_ = -1};
    const auto conservative = co_await live::read_notification_batch(admin.handle(), reader::keys, unknown_counter, false);
    require(conservative.array()[1].array()[0].array()[4].integer() == 1,
        "an unavailable old-server ordinal falls back to conservative stream recovery");
    (void)co_await admin.handle().command("XTRIM", keys[0], "MAXLEN", "0");
    const auto emptied = co_await live::read_notification_batch(admin.handle(), reader::keys, unknown_counter, false);
    require(emptied.array()[1].array()[0].array()[4].integer() == 1,
        "legacy metadata also detects an unread stream that was trimmed completely empty");

    (void)co_await admin.handle().del(available::generation_key);
    const auto reset = co_await live::read_notification_batch(admin.handle(), reader::keys, cursors, false);
    require(!live::matching_notification_generation(reset, generation, reader::keys.size()), "reset between generation lookup and batch read rejects that batch");
    (void)co_await available::generation(admin.handle());
    co_await clean(admin);
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_redis_config();
        if (!config) {
            return 77;
        }
        ruvia::event_loop_pool loops({.loop_count_ = 3});
        const auto admin_loop = loops.loop(0), first_loop = loops.loop(1), second_loop = loops.loop(2);
        ruvia::redis_client admin(admin_loop, *config), first(first_loop, *config), second(second_loop, *config);
        reader_context first_context(first), second_context(second);
        redis_inspector inspector(*config);
        std::optional<ruvia::root_task<void>> first_reader, second_reader;
        loops.start();
        std::exception_ptr failure;
        try {
            admin_loop.start(admin.connect()).get();
            first_loop.start(first.connect()).get();
            second_loop.start(second.connect()).get();
            admin_loop.start(clean(admin)).get();
            admin_loop.start(verify_atomic_batches(admin)).get();
            first_loop.start(subscribe(first_context)).get();
            second_loop.start(subscribe(second_context)).get();
            first_reader.emplace(first_loop.start(reader::run(first_context)));
            first_loop.start(initial_log_snapshot(first_context)).get();
            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 1)).get();
            admin_loop.start(publish_change(admin)).get();
            first_loop.start(receive_business_change(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            second_reader.emplace(second_loop.start(reader::run(second_context)));
            second_loop.start(initial_log_snapshot(second_context)).get();
            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();
            admin_loop.start(publish_change(admin)).get();
            first_loop.start(receive_business_change(first_context)).get();
            second_loop.start(receive_business_change(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            admin_loop.start(publish_receipt(admin)).get();
            first_loop.start(receive_receipt(first_context)).get();
            second_loop.start(receive_receipt(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();
            {
                paused_worker first_paused(first_loop), second_paused(second_loop);
                admin_loop.start(trim_notifications(admin, {0})).get();
            }
            first_loop.start(receive_resource_gap(first_context)).get();
            second_loop.start(receive_resource_gap(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();
            admin_loop.start(publish_change(admin)).get();
            first_loop.start(receive_business_change(first_context)).get();
            second_loop.start(receive_business_change(second_context)).get();

            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();
            {
                paused_worker first_paused(first_loop), second_paused(second_loop);
                admin_loop.start(trim_notifications(admin, {1})).get();
            }
            first_loop.start(receive_log_gap(first_context)).get();
            second_loop.start(receive_log_gap(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();
            {
                paused_worker first_paused(first_loop), second_paused(second_loop);
                admin_loop.start(trim_notifications(admin, {2})).get();
            }
            first_loop.start(receive_receipt_gap(first_context)).get();
            second_loop.start(receive_receipt_gap(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            admin_loop.start(publish_tenant_recovery(admin)).get();
            first_loop.start(receive_tenant_recovery(first_context)).get();
            second_loop.start(receive_tenant_recovery(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            admin_loop.start(reset_generation(admin)).get();
            first_loop.start(receive_recovery(first_context)).get();
            second_loop.start(receive_recovery(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();

            const auto disconnected = admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();
            admin_loop.start(disconnect_readers(inspector, disconnected)).get();
            auto first_recovery = first_loop.start(receive_recovery(first_context));
            auto second_recovery = second_loop.start(receive_recovery(second_context));
            first_recovery.get();
            second_recovery.get();
            first_loop.start(quiet(first_context)).get();
            second_loop.start(quiet(second_context)).get();
            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 2)).get();

            auto started = clock_type::now();
            first_context.stop_.request_stop();
            first_reader->get();
            require(clock_type::now() - started < std::chrono::seconds(2), "cancellation promptly retires a blocked read");
            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 1)).get();
            admin_loop.start(publish_change(admin)).get();
            second_loop.start(receive_business_change(second_context)).get();
            first_loop.start(quiet(first_context)).get();
            started = clock_type::now();
            second_context.stop_.request_stop();
            second_reader->get();
            require(clock_type::now() - started < std::chrono::seconds(2), "the other reader independently cancels its own blocked read");
            (void)admin_loop.start(wait_blocked(admin, inspector, config->database_, 0)).get();
        } catch (...) {
            failure = std::current_exception();
        }
        first_context.stop_.request_stop();
        second_context.stop_.request_stop();
        if (first_reader && first_reader->valid()) {
            first_reader->get();
        }
        if (second_reader && second_reader->valid()) {
            second_reader->get();
        }
        admin_loop.start(clean(admin)).get();
        first_loop.start(first.shutdown()).get();
        second_loop.start(second.shutdown()).get();
        admin_loop.start(admin.shutdown()).get();
        loops.stop();
        loops.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Combined notification reader: one BLOCK per worker, atomic cursor batches, trim recovery for SSE/log tails/receipts, no unrelated recovery, generation recovery and cancellation passed\n";
    return 0;
}
