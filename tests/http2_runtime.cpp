#include <fstream>
#include <iostream>
#include <thread>

#include <asio/read.hpp>

#include "node/data/data_plane.h"
#include "node/runtime/config_activation.h"
#include "node/runtime/loop_shutdown.h"
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
                    } else if (auto* body = event->message_body_chunk()) {
                        self->requests_.at(body->stream_id()).bytes_ += body->bytes().size();
                    } else if (const auto* end = event->message_end()) {
                        const auto id = end->stream_id();
                        auto& request = self->requests_.at(id);
                        const auto& path = request.path_;
                        ruvia::http_response response;
                        response.status(ruvia::http_status::ok);
                        response.header("Cache-Control", "public, max-age=600");
                        response.header("Content-Type", "text/plain");
                        response.header("X-Origin-Protocol", "h2");
                        response.body(path == "/large" ? std::string(512 * 1024, 'x') : path == "/upload" ? std::to_string(request.bytes_)
                                                                                                          : path);
                        FLEXEDGE_REQUIRE(self->protocol_.submit_buffered_response(id, response) == ruvia::http2_submit_status::accepted);
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
        std::optional<ruvia::http2_request_head_event> head_{};
    };
    std::map<std::uint32_t, request> requests_;
    std::atomic<unsigned>& count_;
    ruvia::http2_connection protocol_;
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
    std::vector<std::pair<std::string, std::string>> headers_{};
};
template <class stream_type>
std::vector<received> requests(stream_type& socket, const std::vector<std::string>& paths, bool cancel = false, std::string_view authority = "www.example.com", std::string_view upload = {}) {
    auto client = ruvia::http2_connection::client();
    std::map<std::uint32_t, std::size_t> ids;
    std::vector<received> responses(paths.size());
    std::size_t ended{};
    for (std::size_t i = 0; i < paths.size(); ++i) {
        const auto submitted = client.submit_request_head(ruvia::http2_regular_request_head_view{
            .method_ = upload.empty() ? "GET" : "POST", .authority_ = authority, .target_ = std::string_view(paths[i]), .content_ = upload.empty() ? ruvia::http2_request_content::none() : ruvia::http2_request_content::known_length(upload.size())});
        FLEXEDGE_REQUIRE(submitted.submitted());
        ids[submitted.submitted()->stream_id()] = i;
        if (!upload.empty()) {
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
        const auto size = socket.read_some(asio::buffer(input));
        FLEXEDGE_REQUIRE(client.feed(std::string_view(input.data(), size)) != ruvia::http2_feed_result::protocol_failure);
        while (auto event = client.next_event()) {
            if (const auto* head = event->response_head()) {
                responses[ids.at(head->stream_id())].status_ = head->head().status().value();
                for (const auto& field : head->head().headers()) {
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
ruvia::task<void> apply(data_plane& plane, const flexedge::testing::TestConfig& config) {
    auto pending = co_await config_activation::prepare(plane, std::make_shared<const compiled_config>(config.active, config.objects));
    co_await pending.commit();
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
    data_plane plane(workers, control.loop(0), runtime, logs);
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
    const auto edge_identity = flexedge::testing::make_identity("www.example.com", 9);
    config.mutableCertificate(0)->set_certificate_chain_pem(edge_identity.certificate_);
    config.mutableCertificate(0)->set_private_key_pem(edge_identity.private_key_);
    config.node_spec.mutable_endpoints(0)->set_http_port(port);
    config.node_spec.mutable_endpoints(0)->set_https_port(tls_port);
    auto* site = config.mutableWebsite();
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
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto replay = requests(socket, {"/large"});
        FLEXEDGE_REQUIRE(replay[0].status_ == 200 && replay[0].body_ == std::string(512 * 1024, 'x'));
        FLEXEDGE_REQUIRE(plain.count() == count);
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const auto replay = requests(socket, {"/large", "/first"}, true);
        FLEXEDGE_REQUIRE(replay[1].status_ == 200 && replay[1].body_ == "/first");
        FLEXEDGE_REQUIRE(plain.count() == count);
    }
    {
        asio::ip::tcp::socket socket(clients);
        socket.connect({asio::ip::address_v4::loopback(), port});
        const std::string body(256 * 1024, 'p');
        const auto result = requests(socket, {"/upload"}, false, "www.example.com", body);
        FLEXEDGE_REQUIRE(result[0].status_ == 200 && result[0].body_ == std::to_string(body.size()));
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
        const auto result = requests(socket, {"/cancel", "/tls"}, true);
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
    config.setGeneration(4);
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
        config.setGeneration(5);
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
        verify("tiered");
        verify("disk");
        std::cout << "HTTP/2 ingress, origin and cache passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
