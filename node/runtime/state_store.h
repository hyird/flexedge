#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "node/proto/artifact.h"
#include "node/proto/edge_control.pb.h"
#include "node/proto/schema_version.h"
#include "node/runtime/state_cipher.h"
#include "node/runtime/secure_file.h"

namespace flexedge::node {

struct PersistentState final {
    v2::ActiveState active;
    std::vector<v2::DeliveryObject> objects;
};

class StateStore final {
  public:
    StateStore(std::filesystem::path directory, std::string_view credentialSecret)
        : directory_(std::move(directory)),
          cipher_(std::make_shared<StateCipher>(credentialSecret)) {}

    [[nodiscard]] PersistentState load() const {
        PersistentState state;
        const auto activeBytes = readEncrypted(directory_ / "active" / "state.pb");
        if (!activeBytes) {
            return state;
        }
        if (!state.active.ParseFromString(*activeBytes)) {
            throw std::runtime_error("stored active state is not valid protobuf");
        }
        if (!state.active.has_node_spec() || !state.active.has_release()) {
            throw std::runtime_error("stored active state is incomplete");
        }
        if (state.active.node_spec().content().schema_version() != kNodeSpecSchemaVersion ||
            state.active.release().content().schema_version() != kClusterReleaseSchemaVersion) {
            return {};
        }
        state.objects.reserve(
            static_cast<std::size_t>(state.active.release().content().objects_size()));
        for (const auto& reference : state.active.release().content().objects()) {
            auto object = loadObject(reference.digest_sha256());
            if (!object) {
                throw std::runtime_error("stored active release object is missing");
            }
            state.objects.push_back(std::move(*object));
        }
        return state;
    }

    [[nodiscard]] std::optional<v2::DeliveryObject> loadObject(std::string_view digest) const {
        if (!safeDigest(digest)) {
            throw std::runtime_error("invalid release object digest");
        }
        const auto bytes = readEncrypted(directory_ / "objects" / (std::string(digest) + ".pb"));
        if (!bytes) {
            return std::nullopt;
        }
        v2::DeliveryObject object;
        if (!object.ParseFromString(*bytes) || object.digest_sha256() != digest ||
            !object.has_content() || artifactDigest(object.content()) != digest) {
            throw std::runtime_error("stored release object failed content verification");
        }
        return object;
    }

    void stage(const v2::ActiveState& active,
               const std::vector<v2::DeliveryObject>& objects) const {
        if (!active.has_node_spec() || !active.has_release()) {
            throw std::invalid_argument("cannot stage an incomplete node state");
        }
        for (const auto& object : objects) {
            if (!safeDigest(object.digest_sha256()) || !object.has_content() ||
                artifactDigest(object.content()) != object.digest_sha256()) {
                throw std::runtime_error("cannot stage an invalid release object");
            }
            writeEncrypted(directory_ / "objects" / (object.digest_sha256() + ".pb"),
                           serializeArtifact(object));
        }
        const auto releaseId = active.release().content().release_id();
        if (!safeIdentifier(releaseId)) {
            throw std::runtime_error("invalid release identifier");
        }
        writeEncrypted(directory_ / "releases" / releaseId / "manifest.pb",
                       serializeArtifact(active.release()));
        writeEncrypted(directory_ / "staged" / "state.pb", serializeArtifact(active));
    }

    void activateStaged() const {
        const auto staged = readSecureFile(directory_ / "staged" / "state.pb");
        if (!staged) {
            throw std::runtime_error("staged node state does not exist");
        }
        if (const auto active = readSecureFile(directory_ / "active" / "state.pb"); active) {
            writeSecureFileAtomic(directory_ / "previous" / "state.pb", *active);
        }
        writeSecureFileAtomic(directory_ / "active" / "state.pb", *staged);
        std::error_code ignored;
        std::filesystem::remove(directory_ / "staged" / "state.pb", ignored);
    }

  private:
    [[nodiscard]] static bool safeDigest(std::string_view value) { return isSha256Digest(value); }

    [[nodiscard]] static bool safeIdentifier(std::string_view value) {
        return !value.empty() && value.size() <= 64 &&
               std::ranges::all_of(
                   value, [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '-'; });
    }

    [[nodiscard]] std::optional<std::string>
    readEncrypted(const std::filesystem::path& path) const {
        const auto bytes = readSecureFile(path);
        return bytes ? std::optional<std::string>{cipher_->open(*bytes)} : std::nullopt;
    }

    void writeEncrypted(const std::filesystem::path& path, std::string_view bytes) const {
        writeSecureFileAtomic(path, cipher_->seal(bytes));
    }

    std::filesystem::path directory_;
    std::shared_ptr<const StateCipher> cipher_;
};

} // namespace flexedge::node
