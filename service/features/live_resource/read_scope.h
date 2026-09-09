#pragma once

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#include <ruvia/core/Channel.h>
#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/WebWorker.h>

namespace service::live_resource {

template <typename T> struct ReadOutcome final {
    std::optional<T> value;
    std::exception_ptr failure;
};

// RequestMemory is monotonic, so repeatedly reading through an SSE Context
// retains every row/DTO until disconnection. A worker callback uses the worker's
// reclaiming pool and its own capability scope. Execute on the SAME worker to
// keep pooled DTO destruction and HTTP stream writes on their owning thread.
// Fetch owns all inputs; no request references survive an aborted receive.
template <typename T, typename Fetch> ruvia::Task<T> readOnce(ruvia::Context& c, Fetch fetch) {
    auto [sender, receiver] = ruvia::makeChannel<ReadOutcome<T>>(c.worker(), {.capacity = 1});
    bool posted{};
    for (const auto& worker : ruvia::app().workers()) {
        if (worker.id() != c.worker().id())
            continue;
        posted = worker
                     .post([sender = std::move(sender), fetch = std::move(fetch)](
                               ruvia::WebWorkerContext& read) mutable -> ruvia::Task<void> {
                         ReadOutcome<T> outcome;
                         try {
                             outcome.value.emplace(co_await fetch(read));
                         } catch (...) {
                             outcome.failure = std::current_exception();
                         }
                         (void)sender.send(std::move(outcome));
                         co_return;
                     })
                     .accepted();
        break;
    }
    if (!posted)
        throw std::runtime_error("live resource read worker is unavailable");
    auto outcome = co_await receiver.receiveFor(std::chrono::seconds(30), c.stopToken());
    if (!outcome.hasValue())
        throw std::runtime_error("live resource read interrupted or timed out");
    auto result = std::move(outcome).takeValue();
    if (result.failure)
        std::rethrow_exception(result.failure);
    co_return std::move(*result.value);
}

} // namespace service::live_resource
