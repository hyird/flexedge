#pragma once

#include <exception>
#include <ruvia/core/EventLoopPool.h>

namespace flexedge::node {

// Declare after objects borrowed by loop tasks, before starting either pool.
// Both joins must complete before any of those objects can be destroyed.
class LoopShutdown final {
  public:
    LoopShutdown(ruvia::EventLoopPool& workers, ruvia::EventLoopPool& control)
        : workers_(workers), control_(control) {}
    LoopShutdown(const LoopShutdown&) = delete;
    LoopShutdown& operator=(const LoopShutdown&) = delete;
    ~LoopShutdown() {
        try { stopAndJoin(); } catch (...) { /* Preserve the active exception. */ }
    }

    void stopAndJoin() {
        if (joined_) return;
        workers_.stop();
        control_.stop();
        std::exception_ptr failure;
        try { workers_.join(); } catch (...) { failure = std::current_exception(); }
        try { control_.join(); } catch (...) {
            if (!failure) failure = std::current_exception();
        }
        joined_ = true;
        if (failure) std::rethrow_exception(failure);
    }

  private:
    ruvia::EventLoopPool& workers_;
    ruvia::EventLoopPool& control_;
    bool joined_{};
};

} // namespace flexedge::node
