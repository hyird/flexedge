#pragma once

#include <ruvia/core/task.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "node/proto/control_protocol.h"
#include "node/proto/edge_control.pb.h"
#include "service/features/node_release/catalog.h"

namespace service::node_release {

enum class transfer_error {
    offset_invalid,
    unavailable,
    transfer_invalid,
    read_failed,
};

struct chunk_outcome final {
    std::optional<flexedge::node::v2::NodeReleaseChunk> chunk_;
    transfer_error error_{};
};

// One connection's chunked download of the node binary. The transfer keeps
// the release it started with, so every chunk comes from the same file.
class transfer final {
public:
    template <typename context_type>
    ruvia::task<chunk_outcome> next(context_type& context, const flexedge::node::v2::NodeReleaseRequest& request) {
        if (!state_) {
            if (request.offset() != 0) {
                co_return chunk_outcome{.chunk_ = std::nullopt, .error_ = transfer_error::offset_invalid};
            }
            const auto& release = current(context);
            const auto total_bytes = release->binary().size();
            if (release->binary().digest() != request.digest_sha256() || total_bytes == 0 ||
                total_bytes > flexedge::node::maximum_node_release_bytes) {
                co_return chunk_outcome{.chunk_ = std::nullopt, .error_ = transfer_error::unavailable};
            }
            state_.emplace(state{.release_ = release, .digest_ = request.digest_sha256(), .next_offset_ = 0});
        }
        const auto total_bytes = static_cast<std::uint64_t>(state_->release_->binary().size());
        if (state_->digest_ != request.digest_sha256() || request.offset() != state_->next_offset_ ||
            total_bytes == 0 || total_bytes > flexedge::node::maximum_node_release_bytes) {
            co_return chunk_outcome{.chunk_ = std::nullopt, .error_ = transfer_error::transfer_invalid};
        }
        auto read = co_await context.run_blocking([release = state_->release_, offset = state_->next_offset_] {
            return release->binary().contents_range(offset, flexedge::node::node_release_chunk_bytes);
        });
        auto bytes = std::move(read).value();
        if (bytes.empty()) {
            co_return chunk_outcome{.chunk_ = std::nullopt, .error_ = transfer_error::read_failed};
        }
        flexedge::node::v2::NodeReleaseChunk chunk;
        chunk.set_version(state_->release_->version());
        chunk.set_digest_sha256(state_->digest_);
        chunk.set_total_bytes(total_bytes);
        chunk.set_offset(state_->next_offset_);
        state_->next_offset_ += static_cast<std::uint64_t>(bytes.size());
        chunk.set_data(std::move(bytes));
        if (state_->next_offset_ == total_bytes) {
            state_.reset();
        }
        co_return chunk_outcome{.chunk_ = std::move(chunk), .error_ = {}};
    }

private:
    struct state final {
        snapshot release_;
        std::string digest_;
        std::uint64_t next_offset_{};
    };

    std::optional<state> state_;
};

}  // namespace service::node_release
