#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace flexedge::node {

inline std::optional<std::string> decode_chunked_body(std::string_view wire,
    std::size_t max_decoded_bytes) {
    std::string decoded;
    std::size_t position{};
    for (;;) {
        const auto line_end = wire.find("\r\n", position);
        if (line_end == std::string_view::npos) {
            return std::nullopt;
        }
        auto size_text = wire.substr(position, line_end - position);
        if (const auto extension = size_text.find(';'); extension != std::string_view::npos) {
            size_text = size_text.substr(0, extension);
        }
        if (size_text.empty()) {
            return std::nullopt;
        }
        std::size_t chunk_size{};
        const auto parsed =
            std::from_chars(size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != size_text.data() + size_text.size()) {
            return std::nullopt;
        }
        position = line_end + 2;
        if (chunk_size == 0) {
            if (wire.size() < position + 2 || !wire.ends_with("\r\n")) {
                return std::nullopt;
            }
            return decoded;
        }
        if (decoded.size() > max_decoded_bytes || chunk_size > max_decoded_bytes - decoded.size() ||
            wire.size() - position < 2 || chunk_size > wire.size() - position - 2) {
            return std::nullopt;
        }
        decoded.append(wire.substr(position, chunk_size));
        position += chunk_size;
        if (wire.substr(position, 2) != "\r\n") {
            return std::nullopt;
        }
        position += 2;
    }
}

}  // namespace flexedge::node
