#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/redis/redis.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "service/features/background/keyed_signal_hub.h"

namespace service::node_channel {

inline constexpr std::string_view completion_stream_key{"flexedge:node-channel:v1:report-completions"};

// Only active connection waits live here. Completion records remain in Redis;
// a signal asks the original connection to read its own receipt again.
class report_waits final {
    using waits = service::background::keyed_signal_hub<std::string, std::monostate, service::background::string_key_hash>;

public:
    using subscription = waits::subscription;

    subscription subscribe(const ruvia::worker_handle& worker, std::string_view id) {
        if (id.empty() || waits_.size() >= 4096 || waits_.contains(id)) {
            throw std::runtime_error("node report wait budget exceeded");
        }
        return waits_.subscribe(worker, std::string(id));
    }
    void publish(std::string_view id) {
        waits_.publish(id);
    }
    void recover() {
        waits_.publish_all();
    }

private:
    waits waits_;
};

// Report completions as the Worker notification reader follows them: each
// entry names one completed report.
struct completion_stream final {
    static constexpr std::string_view key{completion_stream_key};
    using value_type = std::string;
    [[nodiscard]] static std::optional<std::string> parse(const ruvia::redis_value& entry) {
        const auto fields = entry.array()[1].array();
        if (fields.size() % 2 != 0) {
            throw std::runtime_error("invalid node report completion fields");
        }
        std::optional<std::string> id;
        for (std::size_t index = 0; index < fields.size(); index += 2) {
            if (fields[index].kind() != ruvia::redis_value::kind_type::string ||
                fields[index + 1].kind() != ruvia::redis_value::kind_type::string) {
                throw std::runtime_error("invalid node report completion field type");
            }
            if (fields[index].string() == "report_id") {
                id = std::string(fields[index + 1].string());
            }
        }
        if (!id || id->empty()) {
            throw std::runtime_error("missing node report completion id");
        }
        return id;
    }
};

// Wakes the calling Worker's connections waiting for their report receipts.
struct report_receipts final {
    using stream = completion_stream;

    template <typename context_type>
    static void deliver(context_type& context, const std::string& id, std::optional<std::int64_t>) {
        context.template worker_state<report_waits>().publish(id);
    }
    // Receipts that raced the reader's cursor snapshot are read again.
    template <typename context_type>
    static void ready(context_type& context) {
        context.template worker_state<report_waits>().recover();
    }
    template <typename context_type>
    static ruvia::task<void> recover(context_type& context, ruvia::redis_handle) {
        context.template worker_state<report_waits>().recover();
        co_return;
    }
};

}  // namespace service::node_channel
