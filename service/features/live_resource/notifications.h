#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>
#include <ruvia/core/timer.h>
#include <ruvia/web/redis/redis.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/live_resource/fanout.h"
#include "service/features/logging/logger.h"
#include "service/utils/redis_reply.h"

// Committed resource changes travel through one Redis stream. Every producer
// (Web Worker request, agent control connection, background worker) appends
// one entry after its transaction commits, on its own Redis connection; every
// Web Worker reads the stream on its own connection and delivers each entry
// to its own subscriptions only. Entries carry identifiers and bounded
// runtime patches, never credentials or configuration revisions.
namespace service::live_resource::notifications {

inline constexpr std::string_view stream_key{"flexedge:resource-notifications:v1"};
// Readers follow the stream head and consume entries as they arrive; the cap
// only bounds Redis memory (about 0.6 KB per entry).
inline constexpr std::int64_t max_entries{10000};
inline constexpr std::uint64_t read_batch_size{128};
inline constexpr auto read_block = std::chrono::seconds(15);
inline constexpr auto read_timeout = std::chrono::seconds(20);
// A producer never waits long on Redis: the business result is committed.
inline constexpr auto send_timeout = std::chrono::seconds(1);
// Per-entry bounds. Beyond them a notification degrades to plain changes or
// a tenant recovery, which subscribers answer with snapshot reads.
inline constexpr std::size_t max_items{32};
inline constexpr std::size_t max_origin_patches{64};
inline constexpr std::size_t max_runtime_bytes{8 * 1024};
inline constexpr std::size_t max_origin_bytes{16 * 1024};
inline constexpr std::size_t max_entry_bytes{256 * 1024};
// Owed recoveries a producer remembers before it owes every tenant one.
inline constexpr std::size_t max_owed_tenants{256};
inline constexpr std::string_view all_tenants{"*"};

[[nodiscard]] inline constexpr std::string_view resource_name(resource value) {
    switch (value) {
        case resource::nodes:
            return "nodes";
        case resource::clusters:
            return "clusters";
        case resource::websites:
            return "websites";
        case resource::dns_zones:
            return "dns_zones";
        case resource::providers:
            return "providers";
        case resource::certificates:
            return "certificates";
        case resource::operations:
            return "operations";
        case resource::overview:
            return "overview";
        case resource::access_history:
            return "access_history";
        case resource::overview_traffic:
            return "overview_traffic";
    }
    throw std::invalid_argument("unsupported live resource");
}

[[nodiscard]] inline std::optional<resource> parse_resource(std::string_view value) {
    for (const auto candidate : {resource::nodes, resource::clusters, resource::websites, resource::dns_zones,
             resource::providers, resource::certificates, resource::operations, resource::overview,
             resource::access_history, resource::overview_traffic}) {
        if (resource_name(candidate) == value) {
            return candidate;
        }
    }
    return std::nullopt;
}

struct resource_change final {
    resource resource_;
    std::string id_;
    friend bool operator==(const resource_change&, const resource_change&) = default;
};
// An attempt recorded for a subtask of task (operation) `task_id_`.
struct task_attempt_change final {
    std::string task_id_;
    task_attempt_notice notice_;
};
struct origin_runtime_change final {
    std::string website_id_;
    std::string payload_;
};

// Everything one committed operation of one tenant changed.
struct resource_notification final {
    std::string tenant_id_;
    std::vector<resource_change> changes_;
    std::vector<task_attempt_change> task_attempts_;
    std::vector<std::string> task_history_resets_;
    // A heartbeat or registration of this node was accepted: its online
    // window restarts. The runtime and origin patches belong to this node.
    std::string node_id_;
    std::string node_runtime_;
    std::vector<origin_runtime_change> origin_runtime_;
    // Node control channels of the tenant re-read their desired state.
    bool node_config_{};
    // The producer could not send earlier notifications of the tenant up to
    // this instant (Unix milliseconds); subscribers read recovery snapshots.
    std::optional<std::int64_t> missed_until_;

    resource_notification() = default;
    explicit resource_notification(std::string_view tenant_id)
        : tenant_id_(tenant_id) {}

    resource_notification& changed(resource value, std::string_view id = {}) {
        resource_change change{value, std::string(id)};
        if (std::find(changes_.begin(), changes_.end(), change) == changes_.end()) {
            changes_.push_back(std::move(change));
        }
        return *this;
    }
    resource_notification& task_attempt(std::string_view task_id, task_attempt_notice notice) {
        task_attempts_.push_back({std::string(task_id), std::move(notice)});
        return *this;
    }
    resource_notification& task_history_reset(std::string_view task_id) {
        task_history_resets_.emplace_back(task_id);
        return *this;
    }
    resource_notification& node_seen(std::string_view node_id) {
        node_id_ = std::string(node_id);
        return *this;
    }
    resource_notification& node_runtime(std::string_view node_id, std::string payload) {
        node_id_ = std::string(node_id);
        node_runtime_ = std::move(payload);
        return *this;
    }
    resource_notification& origin_runtime(std::string_view website_id, std::string payload) {
        origin_runtime_.push_back({std::string(website_id), std::move(payload)});
        return *this;
    }
    resource_notification& node_config() {
        node_config_ = true;
        return *this;
    }
    [[nodiscard]] bool empty() const noexcept {
        return changes_.empty() && task_attempts_.empty() && task_history_resets_.empty() && node_id_.empty() &&
               origin_runtime_.empty() && !node_config_ && !missed_until_;
    }
};

namespace detail {

inline constexpr std::string_view origin_field_prefix{"origin:"};

inline bool contains_separator(std::string_view value) {
    return value.find_first_of(";,:") != std::string_view::npos;
}

template <typename integer_type>
std::optional<integer_type> parse_integer(std::string_view value) {
    integer_type result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return result;
}

inline std::vector<std::string_view> split(std::string_view value, char separator) {
    std::vector<std::string_view> parts;
    if (value.empty()) {
        return parts;
    }
    std::size_t start = 0;
    for (;;) {
        const auto end = value.find(separator, start);
        parts.push_back(value.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) {
            return parts;
        }
        start = end + 1;
    }
}

}  // namespace detail

// Keeps an entry within its bounds: oversized patches turn into changes of
// the patched rows, and lists beyond max_items into one tenant recovery.
[[nodiscard]] inline resource_notification bounded(resource_notification value) {
    if (value.node_runtime_.size() > max_runtime_bytes) {
        value.node_runtime_.clear();
        value.changed(resource::nodes, value.node_id_);
    }
    std::size_t origin_bytes = 0;
    std::vector<origin_runtime_change> origins;
    for (auto& origin : value.origin_runtime_) {
        origin_bytes += origin.payload_.size();
        if (origin.payload_.size() > max_origin_bytes || origins.size() >= max_origin_patches ||
            origin_bytes > max_entry_bytes / 2) {
            value.changed(resource::websites, origin.website_id_);
        } else {
            origins.push_back(std::move(origin));
        }
    }
    value.origin_runtime_ = std::move(origins);
    if (value.changes_.size() > max_items || value.task_attempts_.size() > max_items ||
        value.task_history_resets_.size() > max_items) {
        value.changes_.clear();
        value.task_attempts_.clear();
        value.task_history_resets_.clear();
        value.missed_until_ =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
    }
    return value;
}

// Field/value pairs of one stream entry (after `bounded`).
[[nodiscard]] inline std::vector<std::pair<std::string, std::string>> encode(const resource_notification& value) {
    if (value.tenant_id_.empty() || detail::contains_separator(value.tenant_id_)) {
        throw std::invalid_argument("resource notification requires a tenant");
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back("tenant_id", value.tenant_id_);
    if (!value.changes_.empty()) {
        std::string changes;
        for (const auto& change : value.changes_) {
            if (detail::contains_separator(change.id_)) {
                throw std::invalid_argument("resource notification id is invalid");
            }
            if (!changes.empty()) {
                changes += ';';
            }
            changes += resource_name(change.resource_);
            changes += ':';
            changes += change.id_;
        }
        fields.emplace_back("changes", std::move(changes));
    }
    if (!value.task_attempts_.empty()) {
        std::string attempts;
        for (const auto& attempt : value.task_attempts_) {
            if (attempt.task_id_.empty() || attempt.notice_.subtask_id_.empty() ||
                detail::contains_separator(attempt.task_id_) ||
                detail::contains_separator(attempt.notice_.subtask_id_)) {
                throw std::invalid_argument("task attempt notification is invalid");
            }
            if (!attempts.empty()) {
                attempts += ';';
            }
            attempts += attempt.task_id_ + ',' + attempt.notice_.subtask_id_ + ',' +
                        std::to_string(attempt.notice_.event_id_);
        }
        fields.emplace_back("task_attempts", std::move(attempts));
    }
    if (!value.task_history_resets_.empty()) {
        std::string resets;
        for (const auto& task_id : value.task_history_resets_) {
            if (task_id.empty() || detail::contains_separator(task_id)) {
                throw std::invalid_argument("task history notification is invalid");
            }
            if (!resets.empty()) {
                resets += ';';
            }
            resets += task_id;
        }
        fields.emplace_back("task_history_resets", std::move(resets));
    }
    if (!value.node_id_.empty()) {
        fields.emplace_back("node_id", value.node_id_);
    }
    if (!value.node_runtime_.empty()) {
        fields.emplace_back("node_runtime", value.node_runtime_);
    }
    for (const auto& origin : value.origin_runtime_) {
        fields.emplace_back(std::string(detail::origin_field_prefix) + origin.website_id_, origin.payload_);
    }
    if (value.node_config_) {
        fields.emplace_back("node_config", "1");
    }
    if (value.missed_until_) {
        fields.emplace_back("missed_until", std::to_string(*value.missed_until_));
    }
    return fields;
}

// The entry's notification from its field/value pairs, or nullopt for an
// entry this version cannot use.
[[nodiscard]] inline std::optional<resource_notification> decode(
    std::span<const std::pair<std::string, std::string>> fields) {
    resource_notification value;
    for (const auto& [name_value, field_value] : fields) {
        const std::string_view name = name_value;
        const std::string_view field = field_value;
        if (name == "tenant_id") {
            value.tenant_id_ = std::string(field);
        } else if (name == "changes") {
            for (const auto item : detail::split(field, ';')) {
                const auto colon = item.find(':');
                const auto parsed = parse_resource(item.substr(0, colon));
                if (colon == std::string_view::npos || !parsed) {
                    return std::nullopt;
                }
                value.changes_.push_back({*parsed, std::string(item.substr(colon + 1))});
            }
        } else if (name == "task_attempts") {
            for (const auto item : detail::split(field, ';')) {
                const auto parts = detail::split(item, ',');
                if (parts.size() != 3 || parts[0].empty() || parts[1].empty()) {
                    return std::nullopt;
                }
                const auto event_id = detail::parse_integer<std::int64_t>(parts[2]);
                if (!event_id) {
                    return std::nullopt;
                }
                value.task_attempts_.push_back(
                    {std::string(parts[0]), {.subtask_id_ = std::string(parts[1]), .event_id_ = *event_id}});
            }
        } else if (name == "task_history_resets") {
            for (const auto item : detail::split(field, ';')) {
                value.task_history_resets_.emplace_back(item);
            }
        } else if (name == "node_id") {
            value.node_id_ = std::string(field);
        } else if (name == "node_runtime") {
            value.node_runtime_ = std::string(field);
        } else if (name.starts_with(detail::origin_field_prefix)) {
            value.origin_runtime_.push_back(
                {std::string(name.substr(detail::origin_field_prefix.size())), std::string(field)});
        } else if (name == "node_config") {
            value.node_config_ = field == "1";
        } else if (name == "missed_until") {
            value.missed_until_ = detail::parse_integer<std::int64_t>(field);
            if (!value.missed_until_) {
                return std::nullopt;
            }
        }
    }
    if (value.tenant_id_.empty() || (!value.node_runtime_.empty() && value.node_id_.empty()) ||
        (!value.origin_runtime_.empty() && value.node_id_.empty())) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] inline std::optional<resource_notification> parse_fields(std::span<const ruvia::redis_value> fields) {
    if (fields.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.reserve(fields.size() / 2);
    for (std::size_t index = 0; index < fields.size(); index += 2) {
        if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
            fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
            return std::nullopt;
        }
        pairs.emplace_back(fields[index].string(), fields[index + 1].string());
    }
    return decode(pairs);
}

inline ruvia::task<void> publish(ruvia::redis_handle redis, const resource_notification& value) {
    const auto fields = encode(value);
    const auto maximum_entries = std::to_string(max_entries);
    std::vector<std::string_view> arguments{"XADD", stream_key, "MAXLEN", "~", maximum_entries, "*"};
    arguments.reserve(arguments.size() + fields.size() * 2);
    for (const auto& [name, field] : fields) {
        arguments.push_back(name);
        arguments.push_back(field);
    }
    const auto result =
        co_await redis.command(std::span<const std::string_view>(arguments.data(), arguments.size()));
    service::utils::require_redis_success(result, "could not publish resource notification");
    if (result.kind() != ruvia::redis_value::kind_type::string || result.string().empty()) {
        throw std::runtime_error("unexpected resource notification publish reply");
    }
}

struct entry final {
    std::string id_;
    std::optional<resource_notification> notification_;
};

struct read_batch final {
    std::vector<entry> entries_;
    std::optional<std::string> cursor_;
};

inline std::string current_entry_id(const ruvia::redis_value& value) {
    service::utils::require_redis_success(value, "could not read resource notification cursor");
    if (value.null()) {
        return "0-0";
    }
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected resource notification cursor reply");
    }
    const auto entries = value.array();
    if (entries.empty()) {
        return "0-0";
    }
    const auto item = entries.front().array();
    if (item.size() != 2 || item.front().kind() != ruvia::redis_value::kind_type::string) {
        throw std::runtime_error("unexpected resource notification cursor entry");
    }
    return std::string(item.front().string());
}

// The newest entry id: a reader starting now skips older entries, which its
// fresh subscriptions already observe through their initial snapshots.
inline ruvia::task<std::string> current_cursor(ruvia::redis_handle redis) {
    co_return current_entry_id(co_await redis.command("XREVRANGE", stream_key, "+", "-", "COUNT", "1"));
}

inline read_batch parse_read_result(const ruvia::redis_value& value) {
    service::utils::require_redis_success(value, "could not read resource notifications");
    read_batch output;
    if (value.null()) {
        return output;
    }
    if (value.kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected resource notification read reply");
    }
    const auto streams = value.array();
    if (streams.empty()) {
        return output;
    }
    if (streams.size() != 1 || streams.front().kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected resource notification read reply");
    }
    const auto stream = streams.front().array();
    if (stream.size() != 2 || stream[0].kind() != ruvia::redis_value::kind_type::string ||
        stream[0].string() != stream_key || stream[1].kind() != ruvia::redis_value::kind_type::array) {
        throw std::runtime_error("unexpected resource notification stream reply");
    }
    const auto entries = stream[1].array();
    output.entries_.reserve(entries.size());
    for (const auto& item : entries) {
        if (item.kind() != ruvia::redis_value::kind_type::array || item.array().size() != 2 ||
            item.array()[0].kind() != ruvia::redis_value::kind_type::string ||
            item.array()[1].kind() != ruvia::redis_value::kind_type::array) {
            throw std::runtime_error("unexpected resource notification entry");
        }
        const auto parts = item.array();
        output.cursor_ = std::string(parts[0].string());
        output.entries_.push_back({std::string(parts[0].string()), parse_fields(parts[1].array())});
    }
    return output;
}

inline ruvia::task<read_batch> read(ruvia::redis_handle redis, std::string_view cursor) {
    const auto block_ms = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(read_block).count());
    const auto count = std::to_string(read_batch_size);
    const auto redis_with_timeout = redis.with_options({.timeout_ = read_timeout});
    co_return parse_read_result(co_await redis_with_timeout.command(
        "XREAD", "BLOCK", block_ms, "COUNT", count, "STREAMS", stream_key, cursor));
}

// Owed recoveries are retried from the first 1 s, doubling up to 30 s, while
// anything is owed; nothing waits on a timer otherwise.
inline constexpr auto owed_retry_initial_delay = std::chrono::seconds(1);
inline constexpr auto owed_retry_maximum_delay = std::chrono::seconds(30);

// Owned by one worker (Web Worker state, or the background worker context):
// sends that worker's notifications on that worker's Redis connection. A
// committed operation never fails because of its notification; a failed send
// is logged and the tenant is owed a recovery. The owed recovery entries are
// appended before the next notification, or by `retry_owed` (which the worker
// runs for its lifetime) once Redis answers again, whichever comes first.
// Readers that lost the stream themselves recover on reconnect and skip owed
// recoveries older than that.
class sender final {
public:
    ruvia::task<void> send(ruvia::redis_handle redis, const resource_notification& value) {
        if (value.empty()) {
            co_return;
        }
        try {
            const auto handle = redis.with_options({.timeout_ = send_timeout});
            co_await publish_owed(handle);
            co_await publish(handle, bounded(value));
        } catch (const std::exception& error) {
            owe(value.tenant_id_);
            service::logging::error(std::string("Resource notification could not be sent; subscribers recover once "
                                                "it is retried: ") +
                                    error.what());
        }
    }

    // Runs on the owning worker for its lifetime. Waits without a timer while
    // nothing is owed; while something is owed, appends the owed recovery
    // entries with bounded backoff until Redis accepts them. Ends when `stop`
    // is requested.
    template <typename context_type>
    ruvia::task<void> retry_owed(context_type& context, ruvia::stop_token stop) {
        auto [signal, receiver] = ruvia::make_channel<std::uint64_t>(context.worker(), {.capacity_ = 1});
        owed_signal_.emplace(std::move(signal));
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(owed_retry_initial_delay);
        while (!stop.stop_requested()) {
            if (!owed_until_) {
                const auto woken = co_await receiver.receive(stop);
                if (!woken.has_value()) {
                    break;
                }
                delay = std::chrono::duration_cast<std::chrono::milliseconds>(owed_retry_initial_delay);
                continue;
            }
            if (co_await ruvia::sleep_for(context.worker(), delay, stop) == ruvia::timer_sleep_result::stop_requested) {
                break;
            }
            try {
                co_await publish_owed(context.redis().with_options({.timeout_ = send_timeout, .stop_token_ = stop}));
                delay = std::chrono::duration_cast<std::chrono::milliseconds>(owed_retry_initial_delay);
            } catch (const std::exception&) {
                delay = std::min(delay * 2, std::chrono::duration_cast<std::chrono::milliseconds>(owed_retry_maximum_delay));
            }
        }
        owed_signal_.reset();
    }

    [[nodiscard]] bool owes() const noexcept {
        return owed_until_.has_value();
    }

private:
    // Appends one recovery entry per owed tenant; throws when Redis fails.
    ruvia::task<void> publish_owed(ruvia::redis_handle handle) {
        if (!owed_until_) {
            co_return;
        }
        // Copies: other sends of this worker may run while this one is
        // suspended. Tenants stay owed until their recovery is sent.
        const auto until = *owed_until_;
        const auto generation = owed_generation_;
        const auto tenants = owed_tenants_;
        for (const auto& tenant : tenants) {
            resource_notification recovery(tenant);
            recovery.missed_until_ = until;
            co_await publish(handle, recovery);
        }
        // A failure while suspended is owed again: keep everything for the
        // next attempt, which covers it as well.
        if (owed_generation_ == generation) {
            owed_tenants_.clear();
            owed_until_.reset();
        }
    }

    void owe(std::string_view tenant) {
        ++owed_generation_;
        owed_until_ =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        if (owed_signal_) {
            (void)owed_signal_->send(owed_generation_);
        }
        if (!owed_tenants_.empty() && owed_tenants_.front() == all_tenants) {
            return;
        }
        if (std::find(owed_tenants_.begin(), owed_tenants_.end(), tenant) != owed_tenants_.end()) {
            return;
        }
        if (owed_tenants_.size() >= max_owed_tenants) {
            owed_tenants_.assign(1, std::string(all_tenants));
            return;
        }
        owed_tenants_.emplace_back(tenant);
    }

    std::vector<std::string> owed_tenants_;
    std::optional<std::int64_t> owed_until_;
    std::uint64_t owed_generation_{};
    // Wakes `retry_owed` when a send fails; absent while it is not running.
    std::optional<ruvia::channel_sender<std::uint64_t>> owed_signal_;
};

}  // namespace service::live_resource::notifications

namespace service::live_resource {

// Sends the committed changes of one operation from the calling worker:
// request and Web Worker job contexts use their worker's sender state, the
// background worker context its own sender. Call only after the commit.
template <typename context_type>
ruvia::task<void> notify(context_type& context, const notifications::resource_notification& value) {
    if constexpr (requires { context.resource_notifications(); }) {
        co_await context.resource_notifications().send(context.redis(), value);
    } else {
        co_await context.template worker_state<notifications::sender>().send(context.redis(), value);
    }
}

}  // namespace service::live_resource
