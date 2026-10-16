#pragma once

#include <ruvia/core/channel.h>
#include <ruvia/core/task.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace service::node_channel {

inline constexpr std::string_view completion_stream_key{"flexedge:node-channel:v1:report-completions"};

// Only active connection waits live here. Completion records remain in Redis;
// a signal asks the original connection to read its own receipt again.
class report_waits final {
public:
    class subscription final {
    public:
        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;
        subscription(subscription&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)),
              id_(std::move(other.id_)),
              receiver_(std::move(other.receiver_)) {}
        ~subscription() {
            if (owner_) {
                owner_->subscribers_.erase(id_);
            }
        }
        ruvia::task<ruvia::worker_wait_result<bool>> receive_for(
            std::chrono::steady_clock::duration duration, ruvia::stop_token stop) {
            return receiver_.receive_for(duration, std::move(stop));
        }

    private:
        friend class report_waits;
        subscription(report_waits& owner, std::string id, ruvia::channel_receiver<bool> receiver)
            : owner_(&owner),
              id_(std::move(id)),
              receiver_(std::move(receiver)) {}
        report_waits* owner_;
        std::string id_;
        ruvia::channel_receiver<bool> receiver_;
    };

    subscription subscribe(const ruvia::worker_handle& worker, std::string_view id) {
        if (id.empty() || subscribers_.size() >= 4096 || subscribers_.contains(std::string(id))) {
            throw std::runtime_error("node report wait budget exceeded");
        }
        auto [sender, receiver] = ruvia::make_channel<bool>(worker, {.capacity_ = 1});
        subscribers_.emplace(std::string(id), std::move(sender));
        return subscription(*this, std::string(id), std::move(receiver));
    }
    void publish(std::string_view id) {
        if (const auto found = subscribers_.find(std::string(id)); found != subscribers_.end()) {
            (void)found->second.send(true);
        }
    }
    void recover() {
        for (const auto& [id, sender] : subscribers_) {
            (void)id;
            (void)sender.send(true);
        }
    }

private:
    std::unordered_map<std::string, ruvia::channel_sender<bool>> subscribers_;
};

}  // namespace service::node_channel
