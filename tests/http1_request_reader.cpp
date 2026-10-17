#include "node/data/http1_request_reader.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition)) {                                              \
            throw std::runtime_error("requirement failed: " #condition); \
        }                                                                \
    } while (false)

namespace {
using namespace flexedge::node;

void verify_head_storage_survives_body_growth() {
    const std::string body = std::string(65535, 'b') + '\0';
    const std::string head =
        "POST /upload?key=value HTTP/1.1\r\nHost: header.example.invalid\r\n"
        "X-Pinned: retained-header-value\r\nContent-Length: 65536\r\n\r\n";
    for (const auto first_packet : {1U, 17U, 8192U}) {
        http1_request_reader reader(2 * 1024 * 1024);
        std::string buffer;
        const auto first_size = std::min<std::size_t>(first_packet, head.size());
        buffer.append(head.substr(0, first_size));
        REQUIRE(reader.consume(buffer) == http1_request_read_status::need_more);
        if (first_size < head.size()) {
            buffer.append(head.substr(first_size));
            REQUIRE(reader.consume(buffer) == http1_request_read_status::need_more);
        }
        std::size_t offset{};
        while (offset + 8192 < body.size()) {
            buffer.append(body.substr(offset, 8192));
            offset += 8192;
            REQUIRE(reader.consume(buffer) == http1_request_read_status::need_more);
            REQUIRE(reader.request().target() == "/upload?key=value");
            REQUIRE(reader.request().header("X-Pinned") == "retained-header-value");
        }
        constexpr std::string_view tail{"next-message"};
        buffer.append(body.substr(offset)).append(tail);
        REQUIRE(reader.consume(buffer) == http1_request_read_status::complete);
        REQUIRE(reader.body().has_value());
        REQUIRE(*reader.body() == body);
        REQUIRE(reader.unconsumed_bytes() == tail);
        REQUIRE(reader.message_bytes() == head.size() + body.size());
        // Reusing body storage must not invalidate any accepted header text.
        buffer.assign(128 * 1024, 'x');
        REQUIRE(reader.request().target() == "/upload?key=value");
        REQUIRE(reader.request().header("Host") == "header.example.invalid");
        REQUIRE(reader.request().header("X-Pinned") == "retained-header-value");
    }
}

void verify_fragmented_chunked_request() {
    const std::string head =
        "POST /chunks HTTP/1.1\r\nHost: chunked.example.invalid\r\n"
        "Transfer-Encoding: chunked\r\n\r\n";
    const std::string wire = head + "3\r\nabc\r\n4\r\ndefg\r\n0\r\nX-End: yes\r\n\r\n";
    http1_request_reader reader(2 * 1024 * 1024);
    std::string buffer;
    for (std::size_t offset = 0; offset != wire.size(); ++offset) {
        buffer.push_back(wire[offset]);
        const auto expected = offset + 1 == wire.size() ? http1_request_read_status::complete
                                                        : http1_request_read_status::need_more;
        REQUIRE(reader.consume(buffer) == expected);
    }
    REQUIRE(reader.body().has_value());
    REQUIRE(*reader.body() == "abcdefg");
    REQUIRE(reader.message_bytes() == wire.size());
    REQUIRE(reader.request().target() == "/chunks");
    REQUIRE(reader.request().header("Host") == "chunked.example.invalid");
    REQUIRE(reader.unconsumed_bytes().empty());
}

void verify_upgrade_remainder_and_head_limits() {
    const std::string head =
        "\r\nGET /socket HTTP/1.1\r\nHost: upgrade.example.invalid\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n";
    const std::string tail{"\x81\x02OK\0", 5};
    http1_request_reader reader(1024);
    std::string buffer = head + tail;
    REQUIRE(reader.consume(buffer) == http1_request_read_status::complete);
    REQUIRE(reader.unconsumed_bytes() == tail);
    REQUIRE(reader.message_bytes() == head.size());
    buffer.assign(32768, 'x');
    REQUIRE(reader.request().target() == "/socket");
    REQUIRE(reader.request().header("Upgrade") == "websocket");
    REQUIRE(reader.request().header("Host") == "upgrade.example.invalid");

    http1_request_reader bounded(1024);
    buffer = "POST /too-large HTTP/1.1\r\nHost: limit.invalid\r\nContent-Length: 2048\r\n\r\n";
    REQUIRE(bounded.consume(buffer) == http1_request_read_status::too_large);
}
}  // namespace

int main() {
    try {
        verify_head_storage_survives_body_growth();
        verify_fragmented_chunked_request();
        verify_upgrade_remainder_and_head_limits();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
