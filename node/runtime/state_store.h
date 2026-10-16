#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "node/runtime/secure_file.h"
#include "node/runtime/state_cipher.h"

namespace flexedge::node {

// The local copy of applied state cannot be used by this build: it does not decrypt, parse,
// or reproduce its content digest, or it is incomplete. Only the on-disk cache is affected;
// the control plane resends desired state after the cache is discarded.
class stored_state_unusable final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct persistent_state final {
    v2::ActiveState active_;
    state_contents contents_;
    std::vector<v2::DeliveryObject> objects_;
};

class state_store final {
public:
    state_store(std::filesystem::path directory, std::string_view credential_secret)
        : directory_(std::move(directory)),
          cipher_(std::make_shared<state_cipher>(credential_secret)) {}

    [[nodiscard]] persistent_state load() const {
        persistent_state state;
        const auto active_bytes = read_encrypted(directory_ / "active" / "state.pb");
        if (!active_bytes) {
            return state;
        }
        if (!state.active_.ParseFromString(*active_bytes)) {
            throw stored_state_unusable("stored active state is not valid protobuf");
        }
        if (!state.active_.has_node_spec() || !state.active_.has_release()) {
            throw stored_state_unusable("stored active state is incomplete");
        }
        auto contents = open_state(state.active_);
        if (!contents) {
            throw stored_state_unusable("stored active state failed content verification");
        }
        if (contents->node_spec_.schema_version() != node_spec_schema_version ||
            !supported_cluster_release_schema(contents->release_.schema_version())) {
            return {};
        }
        state.contents_ = std::move(*contents);
        state.objects_.reserve(static_cast<std::size_t>(state.contents_.release_.objects_size()));
        for (const auto& reference : state.contents_.release_.objects()) {
            if (!safe_digest(reference.digest_sha256())) {
                throw stored_state_unusable("stored active release references an invalid digest");
            }
            auto object = load_object(reference.digest_sha256());
            if (!object) {
                throw stored_state_unusable(
                    "stored active release object " + reference.digest_sha256() + " is missing");
            }
            state.objects_.push_back(std::move(*object));
        }
        return state;
    }

    [[nodiscard]] std::optional<v2::DeliveryObject> load_object(std::string_view digest) const {
        if (!safe_digest(digest)) {
            throw std::runtime_error("invalid release object digest");
        }
        const auto bytes = read_encrypted(object_path(digest));
        if (!bytes) {
            return std::nullopt;
        }
        v2::DeliveryObject object;
        if (!object.ParseFromString(*bytes) || object.digest_sha256() != digest ||
            !artifact_matches(digest, object.content())) {
            throw stored_state_unusable(
                "stored release object " + std::string(digest) + " failed content verification");
        }
        return object;
    }

    // Moves an unusable active state into the single previous slot, like activate_staged
    // does for a replaced state, so the next session starts without local state.
    void discard_active() const {
        const auto active = directory_ / "active" / "state.pb";
        if (const auto bytes = read_secure_file(active); bytes) {
            write_secure_file_atomic(directory_ / "previous" / "state.pb", *bytes);
        }
        std::filesystem::remove(active);
    }

    // Objects are content-addressed; a verified copy is fetched again under the same name.
    void discard_object(std::string_view digest) const {
        if (!safe_digest(digest)) {
            throw std::runtime_error("invalid release object digest");
        }
        std::filesystem::remove(object_path(digest));
    }

    void stage(const v2::ActiveState& active,
        const std::vector<v2::DeliveryObject>& objects) const {
        if (!active.has_node_spec() || !active.has_release()) {
            throw std::invalid_argument("cannot stage an incomplete node state");
        }
        for (const auto& object : objects) {
            if (!safe_digest(object.digest_sha256()) ||
                !artifact_matches(object.digest_sha256(), object.content())) {
                throw std::runtime_error("cannot stage an invalid release object");
            }
            write_encrypted(object_path(object.digest_sha256()), serialize_artifact(object));
        }
        write_encrypted(directory_ / "staged" / "state.pb", serialize_artifact(active));
    }

    std::optional<std::string> activate_staged() const {
        const auto staged = read_secure_file(directory_ / "staged" / "state.pb");
        if (!staged) {
            throw std::runtime_error("staged node state does not exist");
        }
        auto active = read_secure_file(directory_ / "active" / "state.pb");
        if (active) {
            write_secure_file_atomic(directory_ / "previous" / "state.pb", *active);
        }
        write_secure_file_atomic(directory_ / "active" / "state.pb", *staged);
        std::error_code ignored;
        std::filesystem::remove(directory_ / "staged" / "state.pb", ignored);
        return active;
    }

    // The previous encrypted bytes are retained by the control-loop activation until all
    // data workers confirm the transaction. An initial activation rolls back to no state.
    void rollback_active(const std::optional<std::string>& previous) const {
        if (previous) {
            write_secure_file_atomic(directory_ / "active" / "state.pb", *previous);
        } else {
            std::filesystem::remove(directory_ / "active" / "state.pb");
        }
    }

private:
    [[nodiscard]] static bool safe_digest(std::string_view value) {
        return flexedge::crypto::is_sha256_digest(value);
    }

    [[nodiscard]] std::filesystem::path object_path(std::string_view digest) const {
        return directory_ / "objects" / (std::string(digest) + ".pb");
    }

    [[nodiscard]] std::optional<std::string>
    read_encrypted(const std::filesystem::path& path) const {
        const auto bytes = read_secure_file(path);
        if (!bytes) {
            return std::nullopt;
        }
        try {
            return cipher_->open(*bytes);
        } catch (const std::runtime_error& error) {
            throw stored_state_unusable(path.filename().string() + ": " + error.what());
        }
    }

    void write_encrypted(const std::filesystem::path& path, std::string_view bytes) const {
        write_secure_file_atomic(path, cipher_->seal(bytes));
    }

    std::filesystem::path directory_;
    std::shared_ptr<const state_cipher> cipher_;
};

}  // namespace flexedge::node
