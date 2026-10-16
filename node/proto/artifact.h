#pragma once

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message_lite.h>

#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/evp.h>

#include "common/hex.h"
#include "common/sha256.h"
#include "node/proto/edge_control.pb.h"

namespace flexedge::node {

inline bool parse_artifact(std::string_view bytes, google::protobuf::MessageLite& message) {
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()));
}

inline std::string serialize_artifact(const google::protobuf::MessageLite& message) {
    std::string bytes;
    bytes.reserve(message.ByteSizeLong());
    {
        google::protobuf::io::StringOutputStream output(&bytes);
        google::protobuf::io::CodedOutputStream coded(&output);
        coded.SetSerializationDeterministic(true);
        if (!message.SerializeToCodedStream(&coded) || coded.HadError()) {
            throw std::runtime_error("could not serialize node protocol artifact");
        }
    }
    return bytes;
}

inline std::string artifact_digest(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned int digest_size{};
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) !=
            1 ||
        digest_size != digest.size()) {
        throw std::runtime_error("could not hash node protocol artifact");
    }
    return flexedge::crypto::hex_encode(digest);
}

[[nodiscard]] inline bool artifact_matches(std::string_view digest, std::string_view bytes) {
    return artifact_digest(bytes) == digest;
}

// Content-addressed envelopes (v2::NodeSpec, v2::ClusterReleaseManifest, v2::DeliveryObject)
// carry the exact content bytes they are addressed by. The digest always covers the bytes as
// received, so fields unknown to the reader never change it.
template <typename envelope_type>
[[nodiscard]] envelope_type pack_artifact(const google::protobuf::MessageLite& content) {
    envelope_type envelope;
    auto bytes = serialize_artifact(content);
    envelope.set_digest_sha256(artifact_digest(bytes));
    envelope.set_content(std::move(bytes));
    return envelope;
}

// Hashes the received bytes and parses them only when they match `digest`.
template <typename content_type>
[[nodiscard]] std::optional<content_type> open_artifact(std::string_view digest,
    std::string_view bytes) {
    content_type content;
    if (!artifact_matches(digest, bytes) || !parse_artifact(bytes, content)) {
        return std::nullopt;
    }
    return content;
}

struct state_contents final {
    v2::NodeSpecContent node_spec_;
    v2::ClusterReleaseContent release_;
};

// Opens both content-addressed parts of a v2::DesiredState or v2::ActiveState. A missing part
// has an empty digest and never verifies.
template <typename state_type>
[[nodiscard]] std::optional<state_contents> open_state(const state_type& state) {
    auto node_spec = open_artifact<v2::NodeSpecContent>(state.node_spec().digest_sha256(),
        state.node_spec().content());
    if (!node_spec) {
        return std::nullopt;
    }
    auto release = open_artifact<v2::ClusterReleaseContent>(state.release().digest_sha256(),
        state.release().content());
    if (!release) {
        return std::nullopt;
    }
    return state_contents{.node_spec_ = std::move(*node_spec), .release_ = std::move(*release)};
}

}  // namespace flexedge::node
