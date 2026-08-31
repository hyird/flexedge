#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include <openssl/evp.h>

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/message_lite.h>

namespace flexedge::node {

inline constexpr bool isSha256Digest(std::string_view value) noexcept {
    if (value.size() != 64) {
        return false;
    }
    for (const char ch : value) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

inline bool parseArtifact(std::string_view bytes, google::protobuf::MessageLite& message) {
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return message.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()));
}

inline std::string serializeArtifact(const google::protobuf::MessageLite& message) {
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

inline std::string artifactDigest(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned int digestSize{};
    if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &digestSize, EVP_sha256(), nullptr) !=
            1 ||
        digestSize != digest.size()) {
        throw std::runtime_error("could not hash node protocol artifact");
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string result(digest.size() * 2, '\0');
    for (std::size_t index = 0; index < digest.size(); ++index) {
        result[index * 2] = kHex[digest[index] >> 4];
        result[index * 2 + 1] = kHex[digest[index] & 0x0f];
    }
    return result;
}

inline std::string artifactDigest(const google::protobuf::MessageLite& message) {
    return artifactDigest(serializeArtifact(message));
}

} // namespace flexedge::node
