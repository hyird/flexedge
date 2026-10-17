#include <ruvia/http/http_content_codec.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/config_fixture.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

namespace {
using namespace flexedge::node;
using clock_type = std::chrono::steady_clock;

std::string payload(std::size_t size, std::string_view pattern = "repeat") {
    std::string result(size, '\0');
    std::uint32_t random = 0x9e3779b9;
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    constexpr std::string_view text = "{\"resource\":\"edge-cache\",\"region\":\"test\",\"active\":true,\"value\":12345}\n";
    for (std::size_t i = 0; i < size; ++i) {
        if (pattern == "mixed") {
            if (i % 256 < 128) {
                result[i] = text[i % text.size()];
            } else {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                result[i] = alphabet[random & 63];
            }
        } else {
            result[i] = static_cast<char>('a' + i % 26);
        }
    }
    return result;
}

class origin final {
public:
    origin(std::size_t size, unsigned delay, std::string_view pattern)
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          bytes_(std::make_shared<const std::string>("HTTP/1.1 200 OK\r\nCache-Control: public, max-age=3600\r\n"
                                                     "Content-Type: application/octet-stream\r\nETag: \"bench-v1\"\r\nConnection: close\r\nContent-Length: " +
                                                     std::to_string(size) + "\r\n\r\n" + payload(size, pattern))),
          delay_(delay) {
        accept();
        thread_ = std::jthread([this] { io_.run(); });
    }
    ~origin() {
        io_.stop();
        thread_.join();
    }
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    std::uint64_t hits() const {
        return hits_;
    }

private:
    void accept() {
        acceptor_.async_accept([this](std::error_code error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            auto peer = std::make_shared<asio::ip::tcp::socket>(std::move(socket));
            auto request = std::make_shared<asio::streambuf>();
            asio::async_read_until(*peer, *request, "\r\n\r\n", [this, peer, request](std::error_code failure, std::size_t) {
                if (failure) {
                    return;
                }
                ++hits_;
                auto timer = std::make_shared<asio::steady_timer>(io_, std::chrono::milliseconds(delay_));
                timer->async_wait([this, peer, timer](std::error_code error) {
                    if (error) {
                        return;
                    }
                    auto bytes = bytes_;
                    asio::async_write(*peer, asio::buffer(*bytes), [peer, bytes](std::error_code, std::size_t) {
                        std::error_code ignored;
                        peer->close(ignored);
                    });
                });
            });
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::shared_ptr<const std::string> bytes_;
    unsigned delay_{};
    std::atomic<std::uint64_t> hits_{};
    std::jthread thread_;
};

ruvia::task<void> activate(data_plane& plane, std::shared_ptr<const compiled_config> config) {
    auto pending = co_await config_activation::prepare(plane, std::move(config));
    co_await pending.commit();
}

void serve(std::uint16_t source_port, std::string storage, const std::string& directory,
    std::string_view transport, bool compression, const std::filesystem::path& identity_directory) {
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [logs, receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 64});
    data_plane plane(workers, control.loop(0), runtime, logs, directory);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    plane.start();
    asio::io_context reservations;
    asio::ip::tcp::acceptor http(reservations, {asio::ip::address_v4::loopback(), 0}), https(reservations, {asio::ip::address_v4::loopback(), 0});
    const auto port = http.local_endpoint().port(), secure_port = https.local_endpoint().port();
    http.close();
    https.close();
    auto config = flexedge::testing::snapshot(false);
    flexedge::testing::pem_identity identity;
    if (!identity_directory.empty()) {
        std::ifstream certificate(identity_directory / "certificate.pem", std::ios::binary);
        std::ifstream key(identity_directory / "private_key.pem", std::ios::binary);
        identity.certificate_.assign(std::istreambuf_iterator<char>(certificate), {});
        identity.private_key_.assign(std::istreambuf_iterator<char>(key), {});
        if (identity.certificate_.empty() || identity.private_key_.empty()) {
            throw std::runtime_error("missing benchmark TLS identity");
        }
    } else {
        identity = flexedge::testing::make_identity("www.example.com", 1);
    }
    config.mutableCertificate(0)->set_certificate_chain_pem(identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(identity.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(secure_port);
    auto* site = config.mutableWebsite();
    site->set_health_check_enabled(false);
    site->set_access_log_enabled(false);
    site->set_response_compression_enabled(compression);
    site->add_response_compression_mime_types("application/octet-stream");
    site->set_minimum_tls_version(transport == "tls13" ? "1.3" : "1.2");
    site->mutable_origins(0)->set_host("127.0.0.1");
    site->mutable_origins(0)->set_port(source_port);
    flexedge::cache::policy policy;
    policy.id_ = "benchmark";
    policy.storage_ = std::move(storage);
    policy.memory_bytes_ = policy.storage_ == "tiered" ? 1024 * 1024 : 128 * 1024 * 1024;
    policy.disk_bytes_ = 1024 * 1024 * 1024;
    policy.max_object_bytes_ = 1024 * 1024;
    flexedge::cache::rule rule;
    rule.id_ = "all";
    rule.name_ = "all";
    rule.match_type_ = "all";
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.finalize();
    control.loop(0).start(activate(plane, std::make_shared<const compiled_config>(config.active, config.objects))).get();
    std::cout << "READY " << (transport == "http" ? port : secure_port) << std::endl;
    std::string command;
    while (std::getline(std::cin, command) && command != "quit") {
    }
    shutdown.stop_and_join();
}

std::string_view response_field(std::string_view head, std::string_view wanted) {
    const auto status_end = head.find("\r\n");
    if (status_end == std::string_view::npos) {
        return {};
    }
    head.remove_prefix(status_end + 2);
    while (!head.empty()) {
        const auto end = head.find("\r\n");
        const auto field = head.substr(0, end);
        const auto colon = field.find(':');
        if (colon != std::string_view::npos && flexedge::route_policy::ascii_equal(field.substr(0, colon), wanted)) {
            return flexedge::route_policy::trim_ows(field.substr(colon + 1));
        }
        if (end == std::string_view::npos) {
            break;
        }
        head.remove_prefix(end + 2);
    }
    return {};
}

std::string unchunk(std::string_view wire) {
    ruvia::http1_chunked_body_decoder decoder;
    std::string result;
    while (!wire.empty()) {
        const auto step = decoder.decode(wire);
        if (const auto* chunk = step.body_chunk()) {
            result.append(chunk->bytes());
            wire.remove_prefix(chunk->consumed_bytes());
        } else if (const auto* end = step.complete()) {
            if (end->consumed_bytes() != wire.size()) {
                throw std::runtime_error("extra bytes after chunked response");
            }
            return result;
        } else {
            throw std::runtime_error("incomplete or invalid chunked response");
        }
    }
    throw std::runtime_error("missing final response chunk");
}

std::string encrypted_request(asio::io_context& io, asio::ssl::context& tls, std::uint16_t port,
    const std::string& target, const std::string& headers, int version, std::atomic<std::size_t>& handshakes) {
    asio::ssl::stream<asio::ip::tcp::socket> stream(io, tls);
    if (SSL_set_tlsext_host_name(stream.native_handle(), "www.example.com") != 1) {
        throw std::runtime_error("could not set benchmark SNI");
    }
    constexpr unsigned char alpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (SSL_set_alpn_protos(stream.native_handle(), alpn, sizeof(alpn)) != 0) {
        throw std::runtime_error("could not set benchmark ALPN");
    }
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    if (SSL_version(stream.native_handle()) != version || SSL_session_reused(stream.native_handle()) != 0) {
        throw std::runtime_error("expected fixed TLS version and full handshake");
    }
    ++handshakes;
    const auto bytes = "GET " + target + " HTTP/1.1\r\nHost: www.example.com\r\n" + headers + "Connection: close\r\n\r\n";
    asio::write(stream, asio::buffer(bytes));
    std::string result;
    std::array<char, 16384> buffer;
    std::error_code error;
    do {
        const auto count = stream.read_some(asio::buffer(buffer), error);
        result.append(buffer.data(), count);
    } while (!error);
    if (error != asio::error::eof && error != asio::ssl::error::stream_truncated) {
        throw std::system_error(error, "could not read benchmark TLS response");
    }
    return result;
}

void load(std::uint16_t port, std::size_t objects, std::size_t size, std::size_t requests, unsigned concurrency, const std::string& prefix, bool require_hit,
    std::string_view transport, std::string_view coding, std::string_view pattern) {
    if (!objects || !requests || !concurrency || concurrency > 256) {
        throw std::runtime_error("invalid load parameters");
    }
    if ((transport != "http" && transport != "tls12" && transport != "tls13") ||
        (coding != "identity" && !configured_coding(coding)) || (pattern != "repeat" && pattern != "mixed")) {
        throw std::runtime_error("invalid transport, coding or payload pattern");
    }
    std::barrier start(static_cast<std::ptrdiff_t>(concurrency + 1));
    std::atomic<std::size_t> next{}, errors{}, hits{};
    std::atomic<std::size_t> http_bytes{}, encoded_body_bytes{}, handshakes{};
    std::mutex mutex;
    std::string first_error;
    std::vector<double> latencies;
    latencies.reserve(requests);
    const auto expected = payload(size, pattern);
    const auto accept = "Accept-Encoding: " + std::string(coding) + "\r\n";
    const auto version = transport == "tls12" ? TLS1_2_VERSION : TLS1_3_VERSION;
    std::vector<std::jthread> threads;
    for (unsigned i = 0; i < concurrency; ++i) {
        threads.emplace_back([&] {
            std::vector<double> local;
            asio::io_context io;
            std::unique_ptr<asio::ssl::context> tls;
            if (transport != "http") {
                tls = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_client);
                tls->set_verify_mode(asio::ssl::verify_none);
                SSL_CTX_set_session_cache_mode(tls->native_handle(), SSL_SESS_CACHE_OFF);
                if (SSL_CTX_set_min_proto_version(tls->native_handle(), version) != 1 ||
                    SSL_CTX_set_max_proto_version(tls->native_handle(), version) != 1) {
                    throw std::runtime_error("could not fix benchmark TLS version");
                }
            }
            start.arrive_and_wait();
            for (;;) {
                const auto index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= requests) {
                    break;
                }
                const auto begin = clock_type::now();
                try {
                    const auto target = prefix + std::to_string(index % objects);
                    const auto response = tls ? encrypted_request(io, *tls, port, target, accept, version, handshakes) : flexedge::testing::request(port, "www.example.com", accept, target);
                    http_bytes += response.size();
                    const auto separator = response.find("\r\n\r\n");
                    if (!response.starts_with("HTTP/1.1 200 ") || separator == std::string::npos) {
                        throw std::runtime_error("invalid response head");
                    }
                    const auto head = std::string_view(response).substr(0, separator + 2);
                    const bool hit = response_field(head, "X-Cache") == "HIT";
                    hits += hit;
                    const auto wire_body = std::string_view(response).substr(separator + 4);
                    std::string framed;
                    const bool chunked = response_field(head, "Transfer-Encoding") == "chunked";
                    if (chunked) {
                        framed = unchunk(wire_body);
                    }
                    const auto body = chunked ? std::string_view(framed) : wire_body;
                    encoded_body_bytes += body.size();
                    if (coding == "identity") {
                        if (!response_field(head, "Content-Encoding").empty() || body != expected) {
                            throw std::runtime_error("invalid identity body");
                        }
                    } else {
                        if (response_field(head, "Content-Encoding") != coding) {
                            throw std::runtime_error("compression was not applied");
                        }
                        const auto decoded = ruvia::decode_http_content(*configured_coding(coding), body, {.max_decoded_bytes_ = size});
                        if (!decoded.decoded() || decoded.decoded()->bytes() != expected) {
                            throw std::runtime_error("decompressed body mismatch");
                        }
                    }
                    if (require_hit && !hit) {
                        throw std::runtime_error("expected a cache hit");
                    }
                } catch (const std::exception& error) {
                    ++errors;
                    std::lock_guard lock(mutex);
                    if (first_error.empty()) {
                        first_error = error.what();
                    }
                }
                local.push_back(std::chrono::duration<double, std::milli>(clock_type::now() - begin).count());
            }
            std::lock_guard lock(mutex);
            latencies.insert(latencies.end(), local.begin(), local.end());
        });
    }
    const auto begin = clock_type::now();
    start.arrive_and_wait();
    threads.clear();
    const auto seconds = std::chrono::duration<double>(clock_type::now() - begin).count();
    std::ranges::sort(latencies);
    const auto percentile = [&](double fraction) { return latencies[(std::min)(latencies.size() - 1, static_cast<std::size_t>(fraction * latencies.size()))]; };
    std::cout << "{\"requests\":" << requests << ",\"seconds\":" << seconds << ",\"rps\":" << requests / seconds
              << ",\"mib_s\":" << requests * static_cast<double>(size) / seconds / 1048576
              << ",\"p50_ms\":" << percentile(.50) << ",\"p95_ms\":" << percentile(.95) << ",\"p99_ms\":" << percentile(.99)
              << ",\"hits\":" << hits << ",\"errors\":" << errors
              << ",\"http_bytes\":" << http_bytes << ",\"encoded_body_bytes\":" << encoded_body_bytes
              << ",\"http_mib_s\":" << http_bytes / seconds / 1048576
              << ",\"encoded_mib_s\":" << encoded_body_bytes / seconds / 1048576
              << ",\"mean_encoded_body_bytes\":" << encoded_body_bytes / static_cast<double>(requests)
              << ",\"tls_full_handshakes\":" << handshakes << "}\n";
    if (errors) {
        throw std::runtime_error("response validation failed: " + first_error);
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string_view(argv[1]) == "identity") {
            const auto identity = flexedge::testing::make_identity("www.example.com", 1);
            const std::filesystem::path directory(argv[2]);
            std::filesystem::create_directories(directory);
            std::ofstream certificate(directory / "certificate.pem", std::ios::binary);
            std::ofstream key(directory / "private_key.pem", std::ios::binary);
            certificate << identity.certificate_;
            key << identity.private_key_;
            if (!certificate || !key) {
                throw std::runtime_error("could not save benchmark TLS identity");
            }
        } else if ((argc == 4 || argc == 5) && std::string_view(argv[1]) == "origin") {
            origin source(std::stoull(argv[2]), static_cast<unsigned>(std::stoul(argv[3])), argc == 5 ? argv[4] : "repeat");
            std::cout << "READY " << source.port() << std::endl;
            std::string command;
            while (std::getline(std::cin, command) && command != "quit") {
                if (command == "stats") {
                    std::cout << source.hits() << std::endl;
                }
            }
        } else if ((argc == 5 || argc == 8) && std::string_view(argv[1]) == "serve") {
            serve(static_cast<std::uint16_t>(std::stoul(argv[2])), argv[3], argv[4], argc == 8 ? argv[5] : "http",
                argc == 8 && std::string_view(argv[6]) == "on", argc == 8 ? argv[7] : "");
        } else if ((argc == 9 || argc == 12) && std::string_view(argv[1]) == "load") {
            load(static_cast<std::uint16_t>(std::stoul(argv[2])), std::stoull(argv[3]), std::stoull(argv[4]), std::stoull(argv[5]),
                static_cast<unsigned>(std::stoul(argv[6])), argv[7], std::string_view(argv[8]) == "hit",
                argc == 12 ? argv[9] : "http", argc == 12 ? argv[10] : "identity", argc == 12 ? argv[11] : "repeat");
        } else {
            throw std::runtime_error("usage: identity <directory> | origin <bytes> <delay_ms> [repeat|mixed] | serve <origin_port> <storage> <directory> [http|tls12|tls13 on|off identity_directory] | load <port> <objects> <bytes> <requests> <concurrency> <prefix> <hit|any> [http|tls12|tls13 identity|gzip|br|zstd repeat|mixed]");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
