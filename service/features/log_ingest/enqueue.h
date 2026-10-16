#pragma once

#include <ruvia/core/task.h>
#include <ruvia/web/context.h>

#include <array>
#include <exception>
#include <string>
#include <string_view>

#include "service/features/background/work_available.h"
#include "service/features/log_ingest/envelope.h"
#include "service/features/log_ingest/queue.h"
#include "service/features/logging/logger.h"

namespace service::log_ingest {

inline constexpr std::string_view work_type{"log-ingest"};

// The enqueuing Worker (the node connection's Worker) wakes its own log
// ingest loop; other Workers find the delivery at their next bounded scan.
template <typename context_type>
void wake_own_ingest(context_type& context) {
    static constexpr std::array work{work_type};
    service::background::work_available::wake_own_claims(context, work);
}

inline ruvia::task<bool> enqueue(ruvia::context& c, std::string_view tenant_id,
    std::string_view node_id, std::string_view agent_id,
    const flexedge::node::v2::LogDelivery& delivery) {
    try {
        if (!valid_delivery(delivery, node_id)) {
            co_return false;
        }
        const auto payload = serialize_envelope(tenant_id, agent_id, delivery);
        if (!(co_await queue::push(c.redis(), payload))) {
            co_return false;
        }
        wake_own_ingest(c);
        co_return true;
    } catch (const std::exception& error) {
        service::logging::error("Log enqueue failed: " + std::string(error.what()));
        co_return false;
    } catch (...) {
        service::logging::error("Log enqueue failed with unknown exception");
        co_return false;
    }
}

}  // namespace service::log_ingest
