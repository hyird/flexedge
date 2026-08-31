#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace flexedge::node {

inline std::optional<std::string> decodeChunkedBody(std::string_view wire,
                                                    std::size_t maxDecodedBytes) {
    std::string decoded;
    std::size_t position{};
    for (;;) {
        const auto lineEnd = wire.find("\r\n", position);
        if (lineEnd == std::string_view::npos) {
            return std::nullopt;
        }
        auto sizeText = wire.substr(position, lineEnd - position);
        if (const auto extension = sizeText.find(';'); extension != std::string_view::npos) {
            sizeText = sizeText.substr(0, extension);
        }
        if (sizeText.empty()) {
            return std::nullopt;
        }
        std::size_t chunkSize{};
        const auto parsed =
            std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), chunkSize, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != sizeText.data() + sizeText.size()) {
            return std::nullopt;
        }
        position = lineEnd + 2;
        if (chunkSize == 0) {
            if (wire.size() < position + 2 || !wire.ends_with("\r\n")) {
                return std::nullopt;
            }
            return decoded;
        }
        if (decoded.size() > maxDecodedBytes || chunkSize > maxDecodedBytes - decoded.size() ||
            wire.size() - position < 2 || chunkSize > wire.size() - position - 2) {
            return std::nullopt;
        }
        decoded.append(wire.substr(position, chunkSize));
        position += chunkSize;
        if (wire.substr(position, 2) != "\r\n") {
            return std::nullopt;
        }
        position += 2;
    }
}

} // namespace flexedge::node
