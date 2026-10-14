#pragma once

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message_lite.h>

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

#include "common/hex.h"
#include "common/sha256.h"

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

inline std::string artifact_digest(const google::protobuf::MessageLite& message) {
    return artifact_digest(serialize_artifact(message));
}

}  // namespace flexedge::node
