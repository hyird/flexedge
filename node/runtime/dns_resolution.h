#pragma once

#include <ruvia/core/async.h>
#include <ruvia/core/event_loop.h>
#include <ruvia/core/stop_token.h>
#include <ruvia/core/task.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

namespace flexedge::node {

using dns_addresses = std::vector<asio::ip::address>;
inline constexpr auto dns_check_interval = std::chrono::seconds(30);

inline dns_addresses resolved_addresses(const asio::ip::tcp::resolver::results_type& endpoints) {
    dns_addresses addresses;
    for (const auto& endpoint : endpoints) {
        addresses.push_back(endpoint.endpoint().address());
    }
    std::ranges::sort(addresses);
    addresses.erase(std::ranges::unique(addresses).begin(), addresses.end());
    return addresses;
}

namespace detail {
// One lookup owns its resolver and deadline. Its completion is delivered only after both
// native callbacks retire; cancellation posted from another thread retains this owner.
class dns_lookup final : public std::enable_shared_from_this<dns_lookup> {
public:
    using completion = std::function<void(std::error_code, dns_addresses)>;
    dns_lookup(ruvia::event_loop loop, std::string host, std::uint16_t port)
        : resolver_(loop.executor()),
          timer_(loop.executor()),
          host_(std::move(host)),
          port_(std::to_string(port)) {}

    void start(completion done) {
        done_ = std::move(done);
        if (stopped_) {
            error_ = asio::error::operation_aborted;
            finish_if_ready();
            return;
        }
        const auto self = shared_from_this();
        try {
            timer_.expires_after(std::chrono::seconds(5));
            initiate([&] {
                timer_.async_wait([self](const std::error_code& error) {
                    if (!error) {
                        self->error_ = asio::error::timed_out;
                        self->resolver_.cancel();
                    }
                    self->completed();
                });
            });
            initiate([&] {
                resolver_.async_resolve(host_, port_, [self](const std::error_code& error, const asio::ip::tcp::resolver::results_type& endpoints) {
                    if (!self->error_) {
                        self->error_ = error;
                        if (!error) {
                            try {
                                self->addresses_ = resolved_addresses(endpoints);
                            } catch (...) {
                                self->error_ = std::make_error_code(std::errc::not_enough_memory);
                            }
                        }
                    }
                    std::error_code ignored;
                    self->timer_.cancel(ignored);
                    self->completed();
                });
            });
        } catch (...) {
            error_ = std::make_error_code(std::errc::io_error);
            resolver_.cancel();
            std::error_code ignored;
            timer_.cancel(ignored);
            finish_if_ready();
        }
    }

    void cancel() noexcept {
        stopped_ = true;
        if (!done_) {
            return;
        }
        error_ = asio::error::operation_aborted;
        resolver_.cancel();
        std::error_code ignored;
        timer_.cancel(ignored);
    }

private:
    template <typename initiation>
    void initiate(initiation&& start) {
        ++pending_;
        try {
            std::forward<initiation>(start)();
        } catch (...) {
            --pending_;
            throw;
        }
    }
    void completed() {
        --pending_;
        finish_if_ready();
    }
    void finish_if_ready() {
        if (pending_ == 0 && done_) {
            auto done = std::move(done_);
            done(error_, std::move(addresses_));
        }
    }

    asio::ip::tcp::resolver resolver_;
    asio::steady_timer timer_;
    std::string host_;
    std::string port_;
    completion done_{};
    std::size_t pending_{};
    bool stopped_{};
    std::error_code error_{};
    dns_addresses addresses_{};
};
}  // namespace detail

inline ruvia::task<std::optional<dns_addresses>> resolve_dns(ruvia::event_loop loop,
    std::string host, std::uint16_t port, ruvia::stop_token stop) {
    if (stop.stop_requested() || !loop.accepting()) {
        co_return std::nullopt;
    }
    std::error_code address_error;
    const auto literal = asio::ip::make_address(host, address_error);
    if (!address_error) {
        co_return dns_addresses{literal};
    }
    auto lookup = std::make_shared<detail::dns_lookup>(loop, std::move(host), port);
    auto retirement = loop.on_stop([lookup]() -> ruvia::task<void> {
        lookup->cancel();
        co_return;
    });
    auto cancellation = stop.register_callback([loop, weak = std::weak_ptr(lookup)]() noexcept {
        try {
            if (const auto retained = weak.lock()) {
                (void)loop.defer_cleanup([retained] { retained->cancel(); });
            }
        } catch (...) {
            loop.report_failure(std::current_exception());
        }
    });
    auto result = co_await ruvia::async_asio<dns_addresses>([lookup](auto done) {
        lookup->start(std::move(done));
    });
    cancellation.reset();
    retirement.reset();
    if (result.error_code() || result.result().empty() || stop.stop_requested()) {
        co_return std::nullopt;
    }
    co_return std::move(result).take_result();
}

}  // namespace flexedge::node
