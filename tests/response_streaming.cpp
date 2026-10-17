#include <ruvia/http/http_content_codec.h>

#include <iostream>
#include <stdexcept>

#include "node/data/origin_response_codec.h"

#define FLEXEDGE_REQUIRE(value)                                                \
    do {                                                                       \
        if (!(value)) throw std::runtime_error("response streaming: " #value); \
    } while (false)

using namespace flexedge::node;
namespace {
v2::Website policy(std::string_view algorithm) {
    v2::Website site;
    site.set_response_compression_enabled(true);
    site.set_response_compression_min_bytes(256);
    site.set_response_compression_max_bytes(0);
    site.add_response_compression_algorithms(std::string(algorithm));
    site.add_response_compression_mime_types("text/*");
    site.add_response_compression_extensions(".txt");
    site.add_response_compression_excluded_extensions(".apk");
    return site;
}
ruvia::http1_client_exchange_state exchange(const v2::Website& site, std::string_view method = "GET") {
    auto request = prepare_origin_request(
        {.method_ = method, .target_ = "/", .headers_ = {}, .body_ = {}, .has_body_ = false},
        site, "example.com", "127.0.0.1", false, false, false);
    FLEXEDGE_REQUIRE(request);
    return request->make_exchange_state();
}
std::string unchunk(std::string_view wire, bool final) {
    ruvia::http1_chunked_body_decoder decoder;
    std::string body;
    while (!wire.empty()) {
        const auto step = decoder.decode(wire);
        if (const auto* chunk = step.body_chunk()) {
            body.append(chunk->bytes());
            wire.remove_prefix(chunk->consumed_bytes());
        } else if (const auto* end = step.complete()) {
            FLEXEDGE_REQUIRE(final && end->consumed_bytes() == wire.size());
            return body;
        } else {
            FLEXEDGE_REQUIRE(step.need_more() && !final);
            wire.remove_prefix(step.need_more()->consumed_bytes());
            FLEXEDGE_REQUIRE(wire.empty());
        }
    }
    FLEXEDGE_REQUIRE(!final);
    return body;
}
std::string body_after_head(const std::string& wire) {
    const auto end = wire.find("\r\n\r\n");
    FLEXEDGE_REQUIRE(end != std::string::npos);
    return wire.substr(end + 4);
}
void compressed_streams() {
    for (const auto algorithm : {"gzip", "br", "zstd"}) {
        auto site = policy(algorithm);
        buffered_bytes_budget budget(64 * 1024 * 1024);
        for (const auto framing : {"length", "chunked", "close"}) {
            {
                origin_response_codec codec(exchange(site), false, nullptr);
                codec.set_compression(site, "/file.txt", algorithm, budget.lease());
                codec.capture_for_cache();
                std::string header =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                    "ETag: \"original\"\r\nVary: Origin\r\nVary: accept-encoding\r\nContent-MD5: invalid-after-transform\r\n";
                if (std::string_view(framing) == "length") {
                    header += "Content-Length: 32768\r\n";
                }
                if (std::string_view(framing) == "chunked") {
                    header += "Transfer-Encoding: chunked\r\nTrailer: X-Final\r\n";
                }
                header += "\r\n";
                FLEXEDGE_REQUIRE(codec.consume_head(header) == origin_response_head_status::ready);
                auto output = codec.take_output();
                FLEXEDGE_REQUIRE(output.find("Transfer-Encoding: chunked\r\n") != std::string::npos);
                FLEXEDGE_REQUIRE(output.find("Content-Length:") == std::string::npos);
                FLEXEDGE_REQUIRE(output.find("Content-MD5:") == std::string::npos);
                FLEXEDGE_REQUIRE(output.find("Trailer:") == std::string::npos);
                FLEXEDGE_REQUIRE(output.find("ETag: W/\"original\"") != std::string::npos);
                FLEXEDGE_REQUIRE(output.find("Vary: Origin, accept-encoding\r\n") != std::string::npos);
                std::string wire = body_after_head(output);
                std::string captured_body = codec.take_cache_output();
                const std::string part(16384, 'x');
                const auto input = std::string_view(framing) == "chunked" ? "4000\r\n" + part + "\r\n" : part;
                FLEXEDGE_REQUIRE(codec.consume_body(input) == origin_response_body_status::accepted);
                output = codec.take_output();
                FLEXEDGE_REQUIRE(!unchunk(output, false).empty());
                FLEXEDGE_REQUIRE(!codec.complete());
                captured_body += codec.take_cache_output();
                wire += output;
                FLEXEDGE_REQUIRE(codec.consume_body(input) == origin_response_body_status::accepted);
                wire += codec.take_output();
                captured_body += codec.take_cache_output();
                if (std::string_view(framing) == "chunked") {
                    for (const char c : std::string("0\r\nX-Final: yes\r\n\r\n")) {
                        FLEXEDGE_REQUIRE(codec.consume_body(std::string_view(&c, 1)) == origin_response_body_status::accepted);
                        wire += codec.take_output();
                        captured_body += codec.take_cache_output();
                    }
                } else if (std::string_view(framing) == "close") {
                    FLEXEDGE_REQUIRE(codec.consume_eof() == origin_response_body_status::accepted);
                    wire += codec.take_output();
                    captured_body += codec.take_cache_output();
                }
                FLEXEDGE_REQUIRE(codec.complete());
                const auto encoded = unchunk(wire, true);
                const auto decoded = ruvia::decode_http_content(*configured_coding(algorithm), encoded,
                    {.max_decoded_bytes_ = part.size() * 2});
                FLEXEDGE_REQUIRE(decoded.decoded() && decoded.decoded()->bytes() == part + part);
                captured_body += codec.take_cache_output();
                const auto* captured = codec.cache_head();
                FLEXEDGE_REQUIRE(captured && captured_body == encoded);
                FLEXEDGE_REQUIRE(cache_header(captured->headers_, "Content-Encoding") == algorithm);
                FLEXEDGE_REQUIRE(cache_header(captured->headers_, "Vary") == "Origin, accept-encoding");
                FLEXEDGE_REQUIRE(codec.consume_body("extra") == origin_response_body_status::failure);
            }
            FLEXEDGE_REQUIRE(budget.used_bytes() == 0);
        }
    }
}
void policies_and_failures() {
    auto site = policy("gzip");
    const auto verify_plain = [&](std::string_view status, std::string_view fields, std::string_view path = "/file.txt",
                                  std::string_view accept = "gzip") {
        origin_response_codec codec(exchange(site), false, nullptr);
        codec.set_compression(site, path, accept, {});
        FLEXEDGE_REQUIRE(codec.consume_head(std::string(status) + "\r\n" + std::string(fields) +
                                            "Content-Length: 1024\r\n\r\n") == origin_response_head_status::ready);
        const auto head = codec.take_output();
        FLEXEDGE_REQUIRE(head.find("Transfer-Encoding: chunked") == std::string::npos);
        FLEXEDGE_REQUIRE(codec.consume_body(std::string(1024, 'p')) == origin_response_body_status::accepted);
        FLEXEDGE_REQUIRE(codec.take_output() == std::string(1024, 'p'));
        FLEXEDGE_REQUIRE(codec.complete());
        return head;
    };
    verify_plain("HTTP/1.1 206 Partial Content", "Content-Type: text/plain\r\n");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\n", "/file.APK");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\nCache-Control: no-transform\r\n");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\nCache-Control: public\r\nCache-Control: no-transform\r\n");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\nContent-Encoding: br\r\n");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\nContent-Encoding: identity\r\nContent-Encoding: br\r\n");
    verify_plain("HTTP/1.1 200 OK", "Content-Type: image/png\r\n", "/image.png");
    FLEXEDGE_REQUIRE(verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\n", "/file.txt", "gzip;q=0, *;q=1")
                         .find("Vary: Accept-Encoding") != std::string::npos);
    site.set_response_compression_max_bytes(512);
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\n");
    site.set_response_compression_max_bytes(0);
    site.set_response_compression_min_bytes(2048);
    verify_plain("HTTP/1.1 200 OK", "Content-Type: text/plain\r\n");
    site.set_response_compression_min_bytes(256);
    FLEXEDGE_REQUIRE(encoding_quality("*;q=0.8", "gzip") == 0.8);
    FLEXEDGE_REQUIRE(encoding_quality("gzip;q=2", "gzip") == 0);
    {
        buffered_bytes_budget budget(1);
        origin_response_codec codec(exchange(site), false, nullptr);
        codec.set_compression(site, "/file.txt", "gzip", budget.lease());
        FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nContent-Length: 1024\r\n\r\n") == origin_response_head_status::ready);
        FLEXEDGE_REQUIRE(codec.take_output().find("Content-Encoding:") == std::string::npos);
        FLEXEDGE_REQUIRE(budget.used_bytes() == 0 && budget.rejected_reservations() > 0);
    }
    for (const auto status : {"HTTP/1.1 200 OK", "HTTP/1.1 304 Not Modified"}) {
        origin_response_codec codec(exchange(site, "HEAD"), false, nullptr);
        codec.set_compression(site, "/file.txt", "gzip", {});
        FLEXEDGE_REQUIRE(codec.consume_head(std::string(status) + "\r\nContent-Length: 8192\r\n\r\n") == origin_response_head_status::ready);
        FLEXEDGE_REQUIRE(codec.complete());
        FLEXEDGE_REQUIRE(codec.take_output().find("Content-Encoding:") == std::string::npos);
    }
    {
        buffered_bytes_budget budget(64 * 1024 * 1024);
        {
            origin_response_codec codec(exchange(site), false, nullptr);
            codec.set_compression(site, "/file.txt", "gzip", budget.lease());
            codec.capture_for_cache();
            FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nContent-Length: 32768\r\n\r\n") == origin_response_head_status::ready);
            (void)codec.take_output();
            FLEXEDGE_REQUIRE(codec.consume_body(std::string(16384, 'x')) == origin_response_body_status::accepted);
            FLEXEDGE_REQUIRE(codec.consume_eof() == origin_response_body_status::failure);
            FLEXEDGE_REQUIRE(!codec.complete());
            FLEXEDGE_REQUIRE(codec.take_output().find("0\r\n\r\n") == std::string::npos);
        }
        FLEXEDGE_REQUIRE(budget.used_bytes() == 0);
    }
    {
        origin_response_codec codec(exchange(site), false, nullptr);
        codec.set_compression(site, "/file.txt", "gzip", {});
        codec.capture_for_cache();
        FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n") == origin_response_head_status::ready);
        (void)codec.take_output();
        FLEXEDGE_REQUIRE(codec.consume_body("ZZ\r\ninvalid\r\n") == origin_response_body_status::failure);
        FLEXEDGE_REQUIRE(!codec.complete());
    }
}
void cache_validation_headers() {
    auto site = policy("gzip");
    const std::array<ruvia::http_header_view, 2> headers{{{"If-None-Match", "client-tag"}, {"If-Modified-Since", "client-date"}}};
    const origin_request_view incoming{.method_ = "GET", .target_ = "/", .headers_ = headers, .body_ = {}, .has_body_ = false};
    auto conditional = prepare_origin_request(incoming, site, "example.com", "127.0.0.1", false, false, false,
        true, nullptr, {"\"cached\"", "Wed, 01 Jan 2025 00:00:00 GMT", true});
    FLEXEDGE_REQUIRE(conditional && conditional->bytes_.find("If-None-Match: \"cached\"\r\n") != std::string::npos);
    FLEXEDGE_REQUIRE(conditional->bytes_.find("client-tag") == std::string::npos && conditional->bytes_.find("If-Modified-Since:") == std::string::npos);
    const auto conditional_date = prepare_origin_request(incoming, site, "example.com", "127.0.0.1", false, false, false,
        true, nullptr, {{}, "Wed, 01 Jan 2025 00:00:00 GMT", true});
    FLEXEDGE_REQUIRE(conditional_date && conditional_date->bytes_.find("If-Modified-Since: Wed, 01 Jan 2025 00:00:00 GMT\r\n") != std::string::npos);
    FLEXEDGE_REQUIRE(conditional_date->bytes_.find("If-None-Match:") == std::string::npos);
}
void independent_cache_directives() {
    auto site = policy("gzip");
    flexedge::cache::rule rule;
    for (const auto control : {"no-cache, max-age=600", "private", "no-store"}) {
        rule.skip_no_cache_ = false;
        origin_response_codec codec(exchange(site), false, nullptr);
        codec.set_cache_rule(rule);
        codec.set_cache_status("MISS");
        codec.set_browser_cache_ttl(120, {200});
        codec.capture_for_cache();
        const auto wire = std::string("HTTP/1.1 200 OK\r\nCache-Control: ") + control + "\r\nContent-Length: 0\r\n\r\n";
        FLEXEDGE_REQUIRE(codec.consume_head(wire) == origin_response_head_status::ready);
        const auto outgoing = codec.take_output();
        if (std::string_view(control).starts_with("no-cache")) {
            FLEXEDGE_REQUIRE(outgoing.find("Cache-Control: public, max-age=120") != std::string::npos);
            FLEXEDGE_REQUIRE(codec.cache_head() && cache_header(codec.cache_head()->headers_, "Cache-Control") == control);
        } else {
            FLEXEDGE_REQUIRE(!codec.cache_head());
            FLEXEDGE_REQUIRE(outgoing.find("X-Cache: BYPASS") != std::string::npos);
            FLEXEDGE_REQUIRE(outgoing.find("max-age=120") == std::string::npos);
        }
    }
    rule.skip_no_cache_ = true;
    origin_response_codec codec(exchange(site), false, nullptr);
    codec.set_cache_rule(rule);
    codec.set_cache_status("MISS");
    codec.capture_for_cache();
    FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nContent-Length: 0\r\n\r\n") == origin_response_head_status::ready);
    FLEXEDGE_REQUIRE(!codec.cache_head() && codec.cache_bypass_reason() == "origin_no_cache");
}
void response_side_sse() {
    for (const bool compression : {false, true}) {
        auto site = policy("gzip");
        origin_response_codec codec(exchange(site), false, nullptr);
        if (compression) {
            codec.set_compression(site, "/file.txt", "gzip", {});
        }
        codec.capture_for_cache();
        codec.set_browser_cache_ttl(600, {200});
        FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nContent-Type: Text/Event-Stream; charset=utf-8\r\n"
                                            "Transfer-Encoding: chunked\r\n\r\n") == origin_response_head_status::ready);
        const auto head = codec.take_output();
        FLEXEDGE_REQUIRE(head.find("Content-Encoding:") == std::string::npos);
        FLEXEDGE_REQUIRE(head.find("Cache-Control: public") == std::string::npos);
        FLEXEDGE_REQUIRE(!codec.cache_head());
        FLEXEDGE_REQUIRE(codec.consume_body("9\r\ndata: x\n\n\r\n") == origin_response_body_status::accepted);
        FLEXEDGE_REQUIRE(codec.take_output() == "9\r\ndata: x\n\n\r\n");
        FLEXEDGE_REQUIRE(codec.take_cache_output().empty() && !codec.complete());
    }
}
void informational_progress() {
    const auto site = policy("gzip");
    const std::string informational =
        "HTTP/1.1 103 Early Hints\r\n"
        "Link: </a.css>; rel=preload\r\n"
        "Link: </b.js>; rel=preload\r\n"
        "Set-Cookie: a=1\r\nSet-Cookie: b=2\r\n"
        "Content-Length: nonsense\r\n"
        "Connection: keep-alive, x-drop\r\n"
        "Connection: Y-Drop\r\n"
        "X-Drop: secret\r\nY-Drop: secret\r\nKeep-Alive: timeout=5\r\n"
        "Cache-Control: no-store\r\nX-Cache: origin-info\r\n\r\n"
        "HTTP/1.1 199 \r\nContent-Length: 0\r\nX-Progress: second\r\n\r\n";
    const std::string final =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nETag: \"final\"\r\n\r\nok";
    // Every boundary exercises both a fragmented head and a coalesced remainder;
    // the downstream parser observes each interim before the final is available.
    for (const bool forward : {true, false}) {
        for (std::size_t split = 0; split <= informational.size(); ++split) {
            origin_response_codec codec(exchange(site), true, nullptr);
            codec.set_forward_informational(forward);
            codec.set_cache_status("MISS");
            codec.set_browser_cache_ttl(120, {200});
            codec.capture_for_cache();
            ruvia::http1_client_response_parser downstream(exchange(site));
            std::size_t interim_count{};
            const auto drain = [&](std::string_view bytes) {
                for (;;) {
                    const auto status = codec.consume_head(bytes);
                    bytes = {};
                    const auto output = codec.take_output();
                    if (status == origin_response_head_status::ready) {
                        const auto parsed = downstream.parse(output);
                        FLEXEDGE_REQUIRE(parsed.parsed() && !parsed.parsed()->plan().informational());
                        FLEXEDGE_REQUIRE(parsed.parsed()->head().status().value() == 200);
                        cache_headers fields;
                        for (const auto& field : parsed.parsed()->head().headers()) {
                            fields.emplace_back(field.name(), field.value());
                        }
                        FLEXEDGE_REQUIRE(cache_header(fields, "Content-Length") == "2");
                        FLEXEDGE_REQUIRE(cache_header(fields, "ETag") == "\"final\"");
                        FLEXEDGE_REQUIRE(cache_header(fields, "Link").empty());
                        FLEXEDGE_REQUIRE(cache_header(fields, "Set-Cookie").empty());
                        FLEXEDGE_REQUIRE(cache_header(fields, "X-Cache") == "MISS");
                        FLEXEDGE_REQUIRE(cache_header(fields, "Cache-Control") == "public, max-age=120");
                        FLEXEDGE_REQUIRE(cache_header(fields, "Strict-Transport-Security") == "max-age=31536000");
                        FLEXEDGE_REQUIRE(output.substr(parsed.parsed()->consumed_bytes()) == "ok");
                        return true;
                    }
                    FLEXEDGE_REQUIRE(status == origin_response_head_status::need_more);
                    FLEXEDGE_REQUIRE(!codec.status_code() && !codec.complete() && !codec.cache_head());
                    FLEXEDGE_REQUIRE(codec.take_cache_output().empty());
                    if (output.empty()) {
                        return false;
                    }
                    FLEXEDGE_REQUIRE(forward);
                    const auto parsed = downstream.parse(output);
                    FLEXEDGE_REQUIRE(parsed.parsed() && parsed.parsed()->plan().informational());
                    FLEXEDGE_REQUIRE(parsed.parsed()->consumed_bytes() == output.size());
                    FLEXEDGE_REQUIRE(parsed.parsed()->head().status().value() == (interim_count == 0 ? 103 : 199));
                    cache_headers fields;
                    std::vector<std::string> cookies;
                    std::string links;
                    for (const auto& field : parsed.parsed()->head().headers()) {
                        fields.emplace_back(field.name(), field.value());
                        if (flexedge::route_policy::ascii_equal(field.name(), "Set-Cookie")) {
                            cookies.emplace_back(field.value());
                        }
                        if (flexedge::route_policy::ascii_equal(field.name(), "Link")) {
                            links.append(field.value());
                        }
                    }
                    for (const auto name : {"Connection", "Keep-Alive", "X-Drop", "Y-Drop",
                             "Content-Length", "Transfer-Encoding", "Strict-Transport-Security"}) {
                        FLEXEDGE_REQUIRE(cache_header(fields, name).empty());
                    }
                    if (interim_count == 0) {
                        FLEXEDGE_REQUIRE(links.find("</a.css>; rel=preload") != std::string::npos);
                        FLEXEDGE_REQUIRE(links.find("</b.js>; rel=preload") != std::string::npos);
                        FLEXEDGE_REQUIRE((cookies == std::vector<std::string>{"a=1", "b=2"}));
                        FLEXEDGE_REQUIRE(cache_header(fields, "Cache-Control") == "no-store");
                        FLEXEDGE_REQUIRE(cache_header(fields, "X-Cache") == "origin-info");
                    } else {
                        FLEXEDGE_REQUIRE(cache_header(fields, "X-Progress") == "second");
                        FLEXEDGE_REQUIRE(cache_header(fields, "Cache-Control").empty());
                        FLEXEDGE_REQUIRE(cache_header(fields, "X-Cache").empty());
                    }
                    ++interim_count;
                }
            };
            FLEXEDGE_REQUIRE(!drain(std::string_view(informational).substr(0, split)));
            const auto first_end = informational.find("\r\n\r\n") + 4;
            const auto completed_interims = static_cast<std::size_t>(split >= first_end) +
                                            static_cast<std::size_t>(split == informational.size());
            FLEXEDGE_REQUIRE(interim_count == (forward ? completed_interims : 0u));
            FLEXEDGE_REQUIRE(drain(informational.substr(split) + final));
            FLEXEDGE_REQUIRE(interim_count == (forward ? 2u : 0u));
            FLEXEDGE_REQUIRE(codec.status_code() == 200 && codec.complete());
            FLEXEDGE_REQUIRE(codec.cache_head() && codec.cache_head()->status_ == 200);
            FLEXEDGE_REQUIRE(cache_header(codec.cache_head()->headers_, "Link").empty());
            FLEXEDGE_REQUIRE(cache_header(codec.cache_head()->headers_, "Set-Cookie").empty());
            FLEXEDGE_REQUIRE(cache_header(codec.cache_head()->headers_, "ETag") == "\"final\"");
            FLEXEDGE_REQUIRE(codec.take_cache_output() == "ok");
        }
    }
}
void connection_field_boundaries() {
    const auto site = policy("gzip");
    std::string wire =
        "HTTP/1.1 200 OK\r\nX-First: sensitive\r\nContent-Length: 2\r\nConnection: ";
    for (unsigned index = 0; index < 128; ++index) {
        wire.append("unused,");
    }
    wire.append(
        " X-FIRST , x-last ,\tX-REPEATED ,, \r\n"
        "Connection: other\r\n"
        "X-Repeated: sensitive-one\r\nx-repeated: sensitive-two\r\n");
    // Exactly 64 fields: the first/last nominated fields exercise both mask edges.
    for (unsigned index = 0; index < 57; ++index) {
        wire.append("X-Fill-").append(std::to_string(index)).append(": safe-").append(std::to_string(index)).append("\r\n");
    }
    wire.append("X-Last: sensitive\r\n\r\nok");
    origin_response_codec codec(exchange(site), false, nullptr);
    codec.capture_for_cache();
    FLEXEDGE_REQUIRE(codec.consume_head(wire) == origin_response_head_status::ready);
    FLEXEDGE_REQUIRE(codec.complete() && codec.cache_head());
    const auto output = codec.take_output();
    ruvia::http1_client_response_parser downstream(exchange(site));
    const auto parsed = downstream.parse(output);
    FLEXEDGE_REQUIRE(parsed.parsed() && parsed.parsed()->head().status().value() == 200);
    cache_headers fields;
    for (const auto& field : parsed.parsed()->head().headers()) {
        fields.emplace_back(field.name(), field.value());
    }
    const auto body = std::string_view(output).substr(parsed.parsed()->consumed_bytes());
    FLEXEDGE_REQUIRE((parsed.parsed()->plan().chunked() ? unchunk(body, true) : std::string(body)) == "ok");
    FLEXEDGE_REQUIRE(codec.take_cache_output() == "ok");
    for (const auto* headers : {static_cast<const cache_headers*>(&fields), &codec.cache_head()->headers_}) {
        for (const auto name : {"X-First", "X-Last", "X-Repeated"}) {
            FLEXEDGE_REQUIRE(cache_header(*headers, name).empty());
        }
        FLEXEDGE_REQUIRE(cache_header(*headers, "X-Fill-0") == "safe-0");
        FLEXEDGE_REQUIRE(cache_header(*headers, "X-Fill-56") == "safe-56");
    }
}

void large_stream() {
    auto site = policy("gzip");
    constexpr std::size_t size = 72 * 1024 * 1024;
    buffered_bytes_budget budget(4 * 1024 * 1024);
    origin_response_codec codec(exchange(site), false, nullptr);
    codec.set_compression(site, "/file.txt", "gzip", budget.lease());
    FLEXEDGE_REQUIRE(codec.consume_head("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n") == origin_response_head_status::ready);
    (void)codec.take_output();
    const std::string part(16384, 'x');
    std::size_t peak{}, bytes{};
    for (std::size_t offset = 0; offset < size; offset += part.size()) {
        FLEXEDGE_REQUIRE(codec.consume_body(part) == origin_response_body_status::accepted);
        auto output = codec.take_output();
        FLEXEDGE_REQUIRE(output.size() < part.size());
        bytes += output.size();
        peak = (std::max)(peak, budget.used_bytes());
    }
    FLEXEDGE_REQUIRE(codec.complete());
    FLEXEDGE_REQUIRE(bytes < size / 10 && peak < budget.maximum_bytes());
    FLEXEDGE_REQUIRE(budget.used_bytes() == 0);
    std::cout << "72 MiB stream: peak codec budget " << peak << " bytes, wire " << bytes << " bytes\n";
}
}  // namespace
int main() {
    try {
        compressed_streams();
        policies_and_failures();
        cache_validation_headers();
        independent_cache_directives();
        response_side_sse();
        informational_progress();
        connection_field_boundaries();
        large_stream();
        std::cout << "response streaming passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
