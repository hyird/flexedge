#include <ruvia/http/http1_chunked_body_decoder.h>
#include <ruvia/http/http1_client_response_parser.h>
#include <ruvia/http/http_content_codec.h>

#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <thread>

#include <asio/read.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
#include "tests/support/cache_directory.h"
#include "tests/support/config_fixture.h"
#include "tests/support/environment.h"
#include "tests/support/http_client.h"
#include "tests/support/tls_identity.h"

#define FLEXEDGE_REQUIRE(value)                                  \
    do {                                                         \
        if (!(value)) {                                          \
            throw std::runtime_error("HTTP/2 runtime: " #value); \
        }                                                        \
    } while (false)
using namespace flexedge::node;
namespace {
constexpr std::uint64_t checksum_initial = 0xcbf29ce484222325ULL;
std::uint64_t update_checksum(std::uint64_t value, std::string_view bytes) {
    for (const unsigned char byte : bytes) {
        value = (value ^ byte) * 0x100000001b3ULL;
    }
    return value;
}
class origin_connection final : public std::enable_shared_from_this<origin_connection> {
public:
    using tls_stream = asio::ssl::stream<asio::ip::tcp::socket>;
    origin_connection(asio::ip::tcp::socket socket, asio::ssl::context* tls, std::atomic<unsigned>& count)
        : stream_(tls ? std::variant<asio::ip::tcp::socket, tls_stream>(std::in_place_type<tls_stream>, std::move(socket), *tls)
                      : std::variant<asio::ip::tcp::socket, tls_stream>(std::in_place_type<asio::ip::tcp::socket>, std::move(socket))),
          count_(count),
          protocol_(ruvia::http2_connection::server()) {}
    void start() {
        if (auto* tls = std::get_if<tls_stream>(&stream_)) {
            tls->async_handshake(asio::ssl::stream_base::server, [self = shared_from_this()](auto error) {if(!error){self->read();self->flush();} });
        } else {
            read();
            flush();
        }
    }

private:
    void read() {
        std::visit([self = shared_from_this()](auto& stream) {
            stream.async_read_some(asio::buffer(self->input_), [self](std::error_code error, std::size_t size) {
                if (error) {
                    return;
                }
                if (self->protocol_.feed(std::string_view(self->input_.data(), size)) == ruvia::http2_feed_result::protocol_failure) {
                    return;
                }
                while (auto event = self->protocol_.next_event()) {
                    if (auto* head = event->request_head()) {
                        ++self->count_;
                        const auto id = head->stream_id();
                        auto& request = self->requests_[id];
                        request.path_ = head->request().target();
                        request.head_.emplace(std::move(*head));
                        if (request.path_ == "/binary") {
                            const auto name = "x-" + std::string(600, 'a');
                            request.checksum_ = update_checksum(
                                request.checksum_, request.head_->request().header(name).value_or(""));
                        }
                        request.early_ = request.path_ == "/early-reset" || request.path_ == "/early-no-reset" || request.path_ == "/early-large";
                        if (request.early_) {
                            ruvia::http_response response;
                            response.status(ruvia::http_status::content_too_large);
                            response.header("Cache-Control", "no-store");
                            response.body(request.path_ == "/early-large" ? std::string(256 * 1024, 'e') : std::string{});
                            FLEXEDGE_REQUIRE(self->protocol_.submit_buffered_response(id, response) == ruvia::http2_submit_status::accepted);
                            if (request.path_ == "/early-reset") {
                                FLEXEDGE_REQUIRE(self->protocol_.submit_reset(id, ruvia::http2_error_code::no_error) == ruvia::http2_submit_status::accepted);
                            }
                        }
                    } else if (auto* body = event->message_body_chunk()) {
                        auto& request = self->requests_.at(body->stream_id());
                        request.bytes_ += body->bytes().size();
                        if (request.path_ == "/binary") {
                            request.checksum_ = update_checksum(request.checksum_, body->bytes());
                        }
                        if (request.early_) {
                            // Keep exact DATA credit: the upload must exhaust the
                            // real peer window while its response remains readable.
                            request.credits_.push_back(body->take_credit());
                        }
                    } else if (const auto* end = event->message_end()) {
                        const auto id = end->stream_id();
                        auto& request = self->requests_.at(id);
                        const auto& path = request.path_;
                        if (!request.early_) {
                            ruvia::http_response response;
                            response.status(ruvia::http_status::ok);
                            response.header("Cache-Control", "public, max-age=600");
                            response.header("Content-Type", "text/plain");
                            response.header("X-Origin-Protocol", "h2");
                            response.body(path == "/large"    ? std::string(512 * 1024, 'x')
                                          : path == "/upload" ? std::to_string(request.bytes_)
                                          : path == "/binary" ? std::to_string(request.checksum_)
                                                              : path);
                            FLEXEDGE_REQUIRE(self->protocol_.submit_buffered_response(id, response) == ruvia::http2_submit_status::accepted);
                        }
                        (void)self->protocol_.release(std::move(*request.head_));
                        self->requests_.erase(id);
                    }
                }
                self->flush();
                self->read();
            });
        },
            stream_);
    }
    void flush() {
        if (writing_ || !protocol_.wants_write()) {
            return;
        }
        writing_ = true;
        output_.clear();
        protocol_.take_output(output_);
        std::visit([self = shared_from_this()](auto& stream) {
            asio::async_write(stream, asio::buffer(self->output_), [self](std::error_code error, std::size_t) {
                self->writing_ = false;
                if (!error) {
                    self->flush();
                }
            });
        },
            stream_);
    }
    std::variant<asio::ip::tcp::socket, tls_stream> stream_;
    struct request final {
        std::string path_{};
        std::size_t bytes_{};
        std::uint64_t checksum_{checksum_initial};
        std::optional<ruvia::http2_request_head_event> head_{};
        std::deque<ruvia::http2_received_data_credit> credits_{};
        bool early_{};
    };
    std::atomic<unsigned>& count_;
    ruvia::http2_connection protocol_;
    std::map<std::uint32_t, request> requests_;
    std::array<char, 16384> input_{};
    std::pmr::string output_;
    bool writing_{};
};
class origin_server final {
public:
    explicit origin_server(const flexedge::testing::pem_identity* identity = nullptr)
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}) {
        if (identity) {
            tls_.emplace(asio::ssl::context::tls_server);
            tls_->use_certificate_chain(asio::buffer(identity->certificate_));
            tls_->use_private_key(asio::buffer(identity->private_key_), asio::ssl::context::pem);
            SSL_CTX_set_alpn_select_cb(tls_->native_handle(), [](SSL*, const unsigned char** out, unsigned char* size, const unsigned char* input, unsigned length, void*) {
                static constexpr unsigned char h2[]{2,'h','2'};
                return SSL_select_next_proto(const_cast<unsigned char**>(out),size,h2,sizeof(h2),input,length)==OPENSSL_NPN_NEGOTIATED?SSL_TLSEXT_ERR_OK:SSL_TLSEXT_ERR_ALERT_FATAL; }, nullptr);
        }
        accept();
        thread_ = std::jthread([this] { io_.run(); });
    }
    ~origin_server() {
        asio::post(io_, [this] {acceptor_.close();io_.stop(); });
        thread_.join();
    }
    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }
    unsigned count() const {
        return count_;
    }

private:
    void accept() {
        acceptor_.async_accept([this](auto error, asio::ip::tcp::socket socket) {
            if (error) {
                return;
            }
            std::make_shared<origin_connection>(std::move(socket), tls_ ? &*tls_ : nullptr, count_)->start();
            accept();
        });
    }
    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::optional<asio::ssl::context> tls_;
    std::jthread thread_;
    std::atomic<unsigned> count_{};
};
struct received final {
    unsigned status_{};
    std::string body_;
    std::string encoding_;
    std::vector<std::pair<std::string, std::string>> headers_{};
    unsigned continues_{};
};
template <class stream_type>
std::vector<received> requests(stream_type& socket, const std::vector<std::string>& paths, bool cancel = false, std::string_view authority = "www.example.com", std::string_view upload = {}, std::string_view encoding = {}, bool expect_continue = false, bool bounded = false) {
    auto client = ruvia::http2_connection::client();
    std::map<std::uint32_t, std::size_t> ids;
    std::vector<received> responses(paths.size());
    std::size_t ended{};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const std::array<ruvia::http_header_view, 1> headers{{{"accept-encoding", encoding}}};
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = upload.empty() ? "GET" : "POST", .authority_ = authority, .target_ = std::string_view(paths[i]), .headers_ = encoding.empty() ? std::span<const ruvia::http_header_view>{} : headers, .content_ = upload.empty() ? ruvia::http2_request_content::none() : ruvia::http2_request_content::known_length(upload.size()), .expectation_ = expect_continue ? ruvia::http_client_request_expectation::continue_value : ruvia::http_client_request_expectation::none});
        FLEXEDGE_REQUIRE(submitted.submitted());
        ids[submitted.submitted()->stream_id()] = i;
        if (!upload.empty() && !expect_continue) {
            const auto result = client.submit_data(submitted.submitted()->stream_id(), upload, ruvia::http2_end_stream::end_stream);
            FLEXEDGE_REQUIRE(result == ruvia::http2_data_submit_status::accepted || result == ruvia::http2_data_submit_status::queued);
        }
        if (cancel && i == 0) {
            (void)client.submit_reset(submitted.submitted()->stream_id(), ruvia::http2_error_code::cancel);
            ++ended;
        }
    }
    std::pmr::string output;
    const auto flush = [&] {if(client.wants_write()){output.clear();client.take_output(output);asio::write(socket,asio::buffer(output));} };
    flush();
    std::array<char, 16384> input{};
    while (ended < paths.size()) {
        if (expect_continue || bounded) {
            FLEXEDGE_REQUIRE(flexedge::testing::waitUntil(
                [&] { return socket.lowest_layer().available() != 0; }, std::chrono::seconds(3)));
        }
        const auto size = socket.read_some(asio::buffer(input));
        FLEXEDGE_REQUIRE(client.feed(std::string_view(input.data(), size)) != ruvia::http2_feed_result::protocol_failure);
        while (auto event = client.next_event()) {
            if (const auto* interim = event->informational_head()) {
                if (interim->head().status().value() == 100 && expect_continue) {
                    FLEXEDGE_REQUIRE(++responses[ids.at(interim->stream_id())].continues_ == 1);
                    const auto result = client.submit_data(interim->stream_id(), upload, ruvia::http2_end_stream::end_stream);
                    FLEXEDGE_REQUIRE(result == ruvia::http2_data_submit_status::accepted || result == ruvia::http2_data_submit_status::queued);
                }
            } else if (const auto* head = event->response_head()) {
                responses[ids.at(head->stream_id())].status_ = head->head().status().value();
                for (const auto& field : head->head().headers()) {
                    if (flexedge::route_policy::ascii_equal(field.name(), "Content-Encoding")) {
                        responses[ids.at(head->stream_id())].encoding_ = field.value();
                    }
                    responses[ids.at(head->stream_id())].headers_.emplace_back(field.name(), field.value());
                }
            } else if (auto* body = event->message_body_chunk()) {
                responses[ids.at(body->stream_id())].body_.append(body->bytes());
            } else if (event->message_end()) {
                ++ended;
            }
        }
        flush();
    }
    return responses;
}
std::string exchange_http1(std::uint16_t port, std::string_view wire) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    asio::write(socket, asio::buffer(wire));
    socket.non_blocking(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::string response;
    std::array<char, 16384> input{};
    for (;;) {
        std::error_code error;
        const auto size = socket.read_some(asio::buffer(input), error);
        response.append(input.data(), size);
        if (error == asio::error::eof) {
            return response;
        }
        FLEXEDGE_REQUIRE(!error || error == asio::error::would_block || error == asio::error::try_again);
        FLEXEDGE_REQUIRE(std::chrono::steady_clock::now() < deadline);
        if (error) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}
ruvia::task<void> apply(data_plane& plane, const flexedge::testing::TestConfig& config) {
    auto pending = co_await config_activation::prepare(plane, std::make_shared<const compiled_config>(config.active, config.objects));
    co_await pending.commit();
}
void verify_pending_stream_close() {
    asio::io_context io;
    buffered_bytes_budget budget(1024);
    auto stream = std::make_shared<http2_stream_state>(
        std::weak_ptr<http2_session>{}, io.get_executor(), 1, budget.lease());
    auto bytes = std::make_shared<std::string>("pending output");
    std::weak_ptr<std::string> retained = bytes;
    unsigned completions{};
    stream->output_ = asio::buffer(*bytes);
    stream->written_ = [bytes, owned = std::make_unique<int>(17), &completions](
                           std::error_code error, std::size_t size) {
        ++completions;
        FLEXEDGE_REQUIRE(error == asio::error::operation_aborted && size == 0);
        FLEXEDGE_REQUIRE(*owned == 17 && *bytes == "pending output");
    };
    bytes.reset();
    stream->close();
    stream->close();
    FLEXEDGE_REQUIRE(completions == 0 && !retained.expired());
    io.run();
    FLEXEDGE_REQUIRE(completions == 1 && retained.expired());
}

void verify(std::string_view storage) {
    auto identity = flexedge::testing::make_identity("localhost", 8);
    const auto ca = std::filesystem::current_path() / "http2-origin-ca.pem";
    {
        std::ofstream output(ca);
        output << identity.certificate_;
    }
    const std::string previous = test_support::environment_variable("SSL_CERT_FILE").value_or("");
#ifdef _WIN32
    _putenv_s("SSL_CERT_FILE", ca.string().c_str());
#else
    setenv("SSL_CERT_FILE", ca.string().c_str(), 1);
#endif
    origin_server plain, secure(&identity);
    ruvia::event_loop_pool workers({.loop_count_ = 2}), control({.loop_count_ = 1});
    runtime_state runtime;
    auto [logs, log_receiver] = ruvia::make_channel<owned_log_batch>(control.loop(0).handle(), {.capacity_ = 128});
    flexedge::testing::temporary_cache_directory cache_directory;
    data_plane plane(workers, control.loop(0), runtime, logs, cache_directory.path_);
    loop_shutdown shutdown(workers, control);
    workers.start();
    control.start();
    plane.start();
    asio::io_context clients;
    asio::ip::tcp::acceptor http(clients, {asio::ip::address_v4::any(), 0}), https(clients, {asio::ip::address_v4::any(), 0});
    const auto port = http.local_endpoint().port(), tls_port = https.local_endpoint().port();
    http.close();
    https.close();
    auto config = flexedge::testing::snapshot(false);
    const auto edge_identity = flexedge::testing::make_identity("*.example.com", 9);
    config.mutableCertificate(0)->set_certificate_chain_pem(edge_identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(edge_identity.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(tls_port);
    auto* site = config.mutableWebsite();
    site->mutable_domains(0)->set_hostname("*.example.com");
    site->set_health_check_enabled(false);
    site->set_response_compression_enabled(false);
    site->set_origin_http_version("http2");
    site->mutable_origins(0)->set_host("127.0.0.1");
    site->mutable_origins(0)->set_port(plain.port());
    flexedge::cache::policy policy;
    policy.id_ = "h2-cache";
    policy.storage_ = storage;
    flexedge::cache::rule rule;
    rule.id_ = "all";
    rule.name_ = "all";
    rule.match_type_ = "all";
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *site->mutable_response_cache());
    config.finalize();
    control.loop(0).start(apply(plane, config)).get();
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto result = requests(socket, {"/first", "/second", "/large"});
        FLEXEDGE_REQUIRE(result[0].body_ == "/first" && result[1].body_ == "/second");
        FLEXEDGE_REQUIRE(result[2].status_ == 200 && result[2].body_ == std::string(512 * 1024, 'x'));
    }
    const auto count = plain.count();
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "www.example.com", {}, "/first").find("X-Cache: HIT") != std::string::npos);
    FLEXEDGE_REQUIRE(plain.count() == count);
    {
        // A cached memory body exceeds the HTTP/2 flow-control window. The common
        // response's head/body buffer sequence must complete without truncation.
        asio::ip::tcp::socket cached(clients);
        cached.connect({asio::ip::address_v4::loopback(), port});
        const auto hit = requests(cached, {"/large"});
        FLEXEDGE_REQUIRE(hit[0].status_ == 200 && hit[0].body_ == std::string(512 * 1024, 'x'));
        FLEXEDGE_REQUIRE(plain.count() == count);
    }
    site->set_revision(site->revision() + 1);
    site->set_response_compression_enabled(true);
    config.setGeneration(4);
    config.finalize();
    control.loop(0).start(apply(plane, config)).get();
    for (const auto algorithm : {"gzip", "br", "zstd"}) {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto result = requests(socket, {"/large"}, false, "www.example.com", {}, algorithm);
        FLEXEDGE_REQUIRE(result[0].status_ == 200 && result[0].encoding_ == algorithm);
        const auto decoded = ruvia::decode_http_content(*configured_coding(algorithm), result[0].body_,
            {.max_decoded_bytes_ = 512 * 1024});
        FLEXEDGE_REQUIRE(decoded.decoded() && decoded.decoded()->bytes() == std::string(512 * 1024, 'x'));
        const auto before_hit = plain.count();
        asio::ip::tcp::socket cached(clients);
        cached.connect({asio::ip::address_v4::loopback(), port});
        const auto hit = requests(cached, {"/large"}, false, "www.example.com", {}, std::string(algorithm) + ";q=1, identity;q=0.1");
        FLEXEDGE_REQUIRE(hit[0].body_ == result[0].body_ && hit[0].encoding_ == algorithm);
        FLEXEDGE_REQUIRE(plain.count() == before_hit);
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto identity_response = requests(socket, {"/large"});
        FLEXEDGE_REQUIRE(identity_response[0].encoding_.empty() && identity_response[0].body_ == std::string(512 * 1024, 'x'));
    }
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "assets.example.com", {}, "/wildcard")
            .ends_with("/wildcard"));
    for (const auto host : {"example.com", "a.b.example.com"}) {
        FLEXEDGE_REQUIRE(flexedge::testing::request(port, host).starts_with("HTTP/1.1 421 "));
    }
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "www.example.com", {}, "/first").ends_with("/first"));
    const auto replay_count = plain.count();
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto replay = requests(socket, {"/large"});
        FLEXEDGE_REQUIRE(replay[0].status_ == 200 && replay[0].body_ == std::string(512 * 1024, 'x'));
        FLEXEDGE_REQUIRE(plain.count() == replay_count);
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto replay = requests(socket, {"/large", "/first"}, true);
        FLEXEDGE_REQUIRE(replay[1].status_ == 200 && replay[1].body_ == "/first");
        FLEXEDGE_REQUIRE(plain.count() == replay_count);
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const std::string body(256 * 1024, 'p');
        const auto result = requests(socket, {"/upload"}, false, "www.example.com", body);
        FLEXEDGE_REQUIRE(result[0].status_ == 200 && result[0].body_ == std::to_string(body.size()));
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const std::string body(256 * 1024, 'e');
        const auto result = requests(socket, {"/upload"}, false, "www.example.com", body, {}, true);
        FLEXEDGE_REQUIRE(result[0].continues_ == 1);
        FLEXEDGE_REQUIRE(result[0].status_ == 200 && result[0].body_ == std::to_string(body.size()));
    }
    {
        std::string binary(4 * 16384 + 7, '\0');
        for (std::size_t index = 0; index < binary.size(); ++index) {
            binary[index] = static_cast<char>((index * 29 + index / 16384) % 251);
        }
        const auto name = "X-" + std::string(600, 'A');
        const std::string value(20000, 'v');
        const auto checksum = std::to_string(update_checksum(
            update_checksum(checksum_initial, value), binary));
        const auto request = prepare_origin_request(
            {.method_ = "POST", .target_ = "/binary", .headers_ = {}, .body_ = {}},
            *site, "www.example.com", "127.0.0.1", false, false, true);
        FLEXEDGE_REQUIRE(request);
        const auto verify_integrity = [&](std::string_view response) {
            ruvia::http1_client_response_parser parser(request->make_exchange_state());
            const auto parsed = parser.parse(response);
            FLEXEDGE_REQUIRE(parsed.parsed() && parsed.parsed()->head().status().value() == 200);
            auto body = response.substr(parsed.parsed()->consumed_bytes());
            std::string decoded;
            if (parsed.parsed()->plan().chunked()) {
                ruvia::http1_chunked_body_decoder decoder;
                for (;;) {
                    const auto step = decoder.decode(body);
                    if (const auto* chunk = step.body_chunk()) {
                        decoded.append(chunk->bytes());
                        body.remove_prefix(chunk->consumed_bytes());
                    } else {
                        FLEXEDGE_REQUIRE(step.complete() && step.complete()->consumed_bytes() == body.size());
                        break;
                    }
                }
            } else {
                const auto* known = parsed.parsed()->plan().known_length();
                FLEXEDGE_REQUIRE(known ? known->content_length() == body.size()
                                       : parsed.parsed()->plan().close_delimited() != nullptr);
                decoded.assign(body);
            }
            FLEXEDGE_REQUIRE(decoded == checksum);
        };
        verify_integrity(flexedge::testing::request(port, "www.example.com",
            name + ": " + value + "\r\nContent-Type: application/octet-stream\r\n",
            "/binary", "POST", binary));
        std::string wire =
            "POST /binary HTTP/1.1\r\nHost: www.example.com\r\n"
            "Content-Type: application/octet-stream\r\n" +
            name + ": " + value +
            "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
        wire.reserve(wire.size() + binary.size() + 256);
        std::size_t offset{};
        for (; offset + 4096 <= binary.size(); offset += 4096) {
            wire.append("1000\r\n").append(binary, offset, 4096).append("\r\n");
        }
        wire.append("7\r\n").append(binary, offset, 7).append("\r\n0\r\n\r\n");
        verify_integrity(exchange_http1(port, wire));
    }
    for (const std::string path : {"/early-reset", "/early-no-reset", "/early-large"}) {
        // Ruvia advertises a 1 MiB request window. Exceed it without returning
        // origin DATA credit, rather than relying on a mock or an empty upload.
        const std::string upload(1536 * 1024, 'p');
        const std::string expected = path == "/early-large" ? std::string(256 * 1024, 'e') : std::string{};
        auto before = plain.count();
        const auto wire = "POST " + path + " HTTP/1.1\r\nHost: www.example.com\r\nContent-Length: " +
                          std::to_string(upload.size()) + "\r\nConnection: close\r\n\r\n" + upload;
        const auto result = exchange_http1(port, wire);
        FLEXEDGE_REQUIRE(result.starts_with("HTTP/1.1 413 "));
        const auto head_end = result.find("\r\n\r\n");
        FLEXEDGE_REQUIRE(head_end != std::string::npos && result.substr(head_end + 4) == expected);
        FLEXEDGE_REQUIRE(plain.count() == before + 1);
        const auto following = "/after-http1" + path;
        FLEXEDGE_REQUIRE(flexedge::testing::request(port, "www.example.com", {}, following).ends_with(following));
        FLEXEDGE_REQUIRE(plain.count() == before + 2);
        {
            asio::ip::tcp::socket socket(clients);
            socket.connect({asio::ip::address_v4::loopback(), port});
            before = plain.count();
            const auto control_path = "/control" + path;
            const auto responses = requests(socket, {path, control_path}, false, "www.example.com", upload, {}, false, true);
            FLEXEDGE_REQUIRE(responses[0].status_ == 413 && responses[0].body_ == expected);
            FLEXEDGE_REQUIRE(responses[1].status_ == 200 && responses[1].body_ == control_path);
            FLEXEDGE_REQUIRE(plain.count() == before + 2);
        }
        const auto after = "/after-http2" + path;
        FLEXEDGE_REQUIRE(flexedge::testing::request(port, "www.example.com", {}, after).ends_with(after));
        FLEXEDGE_REQUIRE(plain.count() == before + 3);
    }
    {
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> socket(clients, tls);
        SSL_set_tlsext_host_name(socket.native_handle(), "assets.example.com");
        constexpr unsigned char h2[]{2, 'h', '2'};
        SSL_set_alpn_protos(socket.native_handle(), h2, sizeof(h2));
        socket.lowest_layer().connect({asio::ip::address_v4::loopback(), tls_port});
        socket.handshake(asio::ssl::stream_base::client);
        const auto result = requests(socket, {"/cancel", "/tls"}, true, "assets.example.com");
        FLEXEDGE_REQUIRE(result[1].status_ == 200 && result[1].body_ == "/tls");
    }
    {
        asio::ssl::context tls(asio::ssl::context::tls_client);
        tls.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<asio::ip::tcp::socket> socket(clients, tls);
        SSL_set_tlsext_host_name(socket.native_handle(), "www.example.com");
        constexpr unsigned char h2[]{2, 'h', '2'};
        SSL_set_alpn_protos(socket.native_handle(), h2, sizeof(h2));
        socket.lowest_layer().connect({asio::ip::address_v4::loopback(), tls_port});
        socket.handshake(asio::ssl::stream_base::client);
        const auto wrong = requests(socket, {"/wrong"}, false, "unknown.example.com");
        FLEXEDGE_REQUIRE(wrong[0].status_ == 421);
    }
    site->set_revision(site->revision() + 1);
    site->set_origin_http_version("auto");
    site->mutable_origins(0)->set_protocol("https");
    site->mutable_origins(0)->set_host("localhost");
    site->mutable_origins(0)->set_port(secure.port());
    config.setGeneration(5);
    config.finalize();
    control.loop(0).start(apply(plane, config)).get();
    FLEXEDGE_REQUIRE(flexedge::testing::request(port, "www.example.com", {}, "/secure-origin").ends_with("/secure-origin"));
    FLEXEDGE_REQUIRE(secure.count() == 1);
    {
        asio::ip::tcp::acceptor source(clients, {asio::ip::address_v4::loopback(), 0});
        const auto origin_port = source.local_endpoint().port();
        std::jthread serving([&] {
            asio::ip::tcp::socket accepted(clients);
            source.accept(accepted);
            std::array<char, 4096> request{};
            accepted.read_some(asio::buffer(request));
            const std::string response =
                "HTTP/1.1 200 OK\r\nConnection: close\r\n"
                "Set-Cookie: session=abc; Path=/\r\n"
                "Set-Cookie: theme=dark; Path=/\r\n"
                "Vary: Accept-Encoding\r\n"
                "Vary: Accept-Language\r\n\r\nclose-delimited";
            asio::write(accepted, asio::buffer(response));
        });
        site->set_revision(site->revision() + 1);
        site->set_origin_http_version("http1");
        site->mutable_origins(0)->set_protocol("http");
        site->mutable_origins(0)->set_host("127.0.0.1");
        site->mutable_origins(0)->set_port(origin_port);
        config.setGeneration(6);
        config.finalize();
        control.loop(0).start(apply(plane, config)).get();
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto result = requests(socket, {"/close"});
        FLEXEDGE_REQUIRE(result[0].status_ == 200 && result[0].body_ == "close-delimited");
        const auto values = [&](std::string_view name) {
            std::vector<std::string> selected;
            for (const auto& [field, value] : result[0].headers_) {
                if (flexedge::route_policy::ascii_equal(field, name)) {
                    selected.push_back(value);
                }
            }
            return selected;
        };
        FLEXEDGE_REQUIRE((values("Set-Cookie") ==
                          std::vector<std::string>{"session=abc; Path=/", "theme=dark; Path=/"}));
        std::vector<std::string> vary;
        for (const auto& value : values("Vary")) {
            std::string_view remaining = value;
            while (!remaining.empty()) {
                const auto comma = remaining.find(',');
                const auto token = flexedge::route_policy::trim_ows(remaining.substr(0, comma));
                if (!token.empty()) {
                    vary.emplace_back(token);
                }
                remaining = comma == std::string_view::npos ? std::string_view{} : remaining.substr(comma + 1);
            }
        }
        FLEXEDGE_REQUIRE((vary == std::vector<std::string>{"Accept-Encoding", "Accept-Language"}));
    }
    shutdown.stop_and_join();
#ifdef _WIN32
    _putenv_s("SSL_CERT_FILE", previous.c_str());
#else
    if (previous.empty()) {
        unsetenv("SSL_CERT_FILE");
    } else {
        setenv("SSL_CERT_FILE", previous.c_str(), 1);
    }
#endif
    std::filesystem::remove(ca);
}
}  // namespace
int main() {
    try {
        verify_pending_stream_close();
        verify("tiered");
        verify("disk");
        std::cout << "HTTP/2 ingress, origin and cache passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
