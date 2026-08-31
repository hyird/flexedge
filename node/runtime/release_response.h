#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "node/proto/artifact.h"

namespace flexedge::node {

struct NodeReleaseResponse final {
    int statusCode{};
    std::string entityTag;
    std::string sha256;
    std::string version;
};

inline bool validNodeReleaseDigest(std::string_view value) noexcept {
    return isSha256Digest(value);
}

inline bool validNodeReleaseVersion(std::string_view value) noexcept {
    return !value.empty() && value.size() <= 64 && std::ranges::all_of(value, [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '.' || ch == '-' || ch == '_' || ch == '+';
    });
}

inline std::string nodeReleaseEntityTag(std::string_view digest) {
    return "\"" + std::string(digest) + "\"";
}

namespace detail {

inline std::string_view trimNodeReleaseHeaderValue(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

inline std::optional<int> parseNodeReleaseStatusLine(std::string_view line) {
    const auto firstSpace = line.find(' ');
    if (firstSpace == std::string_view::npos) {
        return std::nullopt;
    }
    auto status = line.substr(firstSpace + 1);
    const auto secondSpace = status.find(' ');
    if (secondSpace != std::string_view::npos) {
        status = status.substr(0, secondSpace);
    }
    int statusCode{};
    const auto [pointer, error] =
        std::from_chars(status.data(), status.data() + status.size(), statusCode);
    if (error != std::errc{} || pointer != status.data() + status.size()) {
        return std::nullopt;
    }
    return statusCode;
}

inline std::string lowercaseNodeReleaseHeaderName(std::string_view name) {
    std::string result(name);
    std::ranges::transform(result, result.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

struct NodeReleaseHeaderState final {
    NodeReleaseResponse response;
    bool responseStarted{};
    bool duplicate{};
    bool hasEntityTag{};
    bool hasDigest{};
    bool hasVersion{};

    void start(int statusCode) noexcept {
        response = {};
        response.statusCode = statusCode;
        responseStarted = true;
        duplicate = false;
        hasEntityTag = false;
        hasDigest = false;
        hasVersion = false;
    }

    void accept(std::string_view name, std::string_view value) {
        if (name == "etag") {
            duplicate = duplicate || hasEntityTag;
            hasEntityTag = true;
            response.entityTag.assign(value);
        } else if (name == "x-flexedge-node-sha256") {
            duplicate = duplicate || hasDigest;
            hasDigest = true;
            response.sha256.assign(value);
        } else if (name == "x-flexedge-node-version") {
            duplicate = duplicate || hasVersion;
            hasVersion = true;
            response.version.assign(value);
        }
    }

    [[nodiscard]] bool valid() const {
        return responseStarted && !duplicate &&
               (response.statusCode == 200 || response.statusCode == 304) && hasEntityTag &&
               hasDigest && hasVersion && validNodeReleaseDigest(response.sha256) &&
               validNodeReleaseVersion(response.version) &&
               response.entityTag == nodeReleaseEntityTag(response.sha256);
    }
};

} // namespace detail

inline std::optional<NodeReleaseResponse> parseNodeReleaseHeaders(std::string_view headers) {
    detail::NodeReleaseHeaderState state;
    std::size_t offset{};
    while (offset <= headers.size()) {
        const auto end = headers.find('\n', offset);
        auto line = headers.substr(offset, end == std::string_view::npos ? headers.size() - offset
                                                                         : end - offset);
        offset = end == std::string_view::npos ? headers.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        if (line.starts_with("HTTP/")) {
            const auto statusCode = detail::parseNodeReleaseStatusLine(line);
            if (!statusCode) {
                return std::nullopt;
            }
            state.start(*statusCode);
            continue;
        }
        if (!state.responseStarted || line.empty()) {
            continue;
        }

        const auto separator = line.find(':');
        if (separator == std::string_view::npos) {
            continue;
        }
        const auto name = detail::lowercaseNodeReleaseHeaderName(line.substr(0, separator));
        const auto value = detail::trimNodeReleaseHeaderValue(line.substr(separator + 1));
        state.accept(name, value);
    }

    if (!state.valid()) {
        return std::nullopt;
    }
    return std::move(state.response);
}

inline std::optional<NodeReleaseResponse>
readNodeReleaseResponse(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("node release response headers are unavailable");
    }

    std::string headers;
    std::array<char, 4096> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        headers.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (!input.eof()) {
        throw std::runtime_error("failed to read node release response headers");
    }
    return parseNodeReleaseHeaders(headers);
}

} // namespace flexedge::node
