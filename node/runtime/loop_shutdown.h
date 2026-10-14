#pragma once

#include <ruvia/core/event_loop_pool.h>

#include <exception>

namespace flexedge::node {

// Declare after objects borrowed by loop tasks, before starting either pool.
// Both joins must complete before any of those objects can be destroyed.
class loop_shutdown final {
public:
    loop_shutdown(ruvia::event_loop_pool& workers, ruvia::event_loop_pool& control)
        : workers_(workers),
          control_(control) {}
    loop_shutdown(const loop_shutdown&) = delete;
    loop_shutdown& operator=(const loop_shutdown&) = delete;
    ~loop_shutdown() {
        try {
            stop_and_join();
        } catch (...) { /* Preserve the active exception. */
        }
    }

    void stop_and_join() {
        if (joined_) {
            return;
        }
        workers_.stop();
        control_.stop();
        std::exception_ptr failure;
        try {
            workers_.join();
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            control_.join();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
        joined_ = true;
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

private:
    ruvia::event_loop_pool& workers_;
    ruvia::event_loop_pool& control_;
    bool joined_{};
};

}  // namespace flexedge::node
