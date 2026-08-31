#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/http/Http2Connection.h>

#include "node/data/http_listener.h"
#include "node/data/chunked_body.h"
#include "node/data/chunked_wire_tracker.h"
#include "node/data/health_supervisor.h"
#include "node/data/https_listener.h"
#include "node/data/origin_selection.h"
#include "node/data/origin_health.h"
#include "node/runtime/http_redirect.h"
#include "node/runtime/node_credentials.h"
#include "node/proto/schema_version.h"
#include "node/runtime/release_response.h"
#include "node/runtime/self_updater.h"
#include "node/runtime/runtime_state.h"
#include "node/runtime/state_cipher.h"
#include "node/runtime/state_store.h"
#include "node/runtime/upgrade_record.h"

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("requirement failed: " #condition);                           \
    } while (false)

namespace {

struct TestConfig final {
    flexedge::node::v2::ActiveState active;
    std::vector<flexedge::node::v2::DeliveryObject> objects;

    flexedge::node::v2::Website* mutableWebsite() {
        for (auto& object : objects) {
            if (object.content().has_website()) {
                return object.mutable_content()->mutable_website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    const flexedge::node::v2::Website& website() const {
        for (const auto& object : objects) {
            if (object.content().has_website()) {
                return object.content().website();
            }
        }
        throw std::runtime_error("test website object is missing");
    }

    flexedge::node::v2::Certificate* mutableCertificate(std::size_t index) {
        for (auto& object : objects) {
            if (!object.content().has_certificate()) {
                continue;
            }
            if (index == 0) {
                return object.mutable_content()->mutable_certificate();
            }
            --index;
        }
        throw std::runtime_error("test certificate object is missing");
    }

    flexedge::node::v2::Certificate* addCertificate() {
        flexedge::node::v2::DeliveryObject object;
        object.mutable_content()->mutable_certificate();
        objects.push_back(std::move(object));
        return objects.back().mutable_content()->mutable_certificate();
    }

    void setGeneration(std::int64_t generation) {
        active.mutable_node_spec()->mutable_content()->set_revision(generation);
        active.mutable_release()->mutable_content()->set_generation(generation);
        active.mutable_release()->mutable_content()->set_release_id("release-" +
                                                                    std::to_string(generation));
    }

    void duplicateWebsite() {
        flexedge::node::v2::DeliveryObject duplicate;
        *duplicate.mutable_content()->mutable_website() = website();
        objects.push_back(std::move(duplicate));
    }

    void finalize() {
        for (auto& object : objects) {
            if (object.content().has_certificate()) {
                object.set_digest_sha256(flexedge::node::artifactDigest(object.content()));
            }
        }
        auto* website = mutableWebsite();
        for (auto& domain : *website->mutable_domains()) {
            if (!domain.https_enabled()) {
                continue;
            }
            bool resolved = false;
            for (const auto& object : objects) {
                resolved = resolved || (object.content().has_certificate() &&
                                        object.digest_sha256() == domain.certificate_digest());
            }
            if (resolved) {
                continue;
            }
            for (const auto& object : objects) {
                if (object.content().has_certificate()) {
                    domain.set_certificate_digest(object.digest_sha256());
                    break;
                }
            }
        }
        auto* manifest = active.mutable_release()->mutable_content();
        manifest->clear_objects();
        for (auto& object : objects) {
            object.set_digest_sha256(flexedge::node::artifactDigest(object.content()));
            auto* reference = manifest->add_objects();
            reference->set_kind(object.content().has_website()
                                    ? flexedge::node::v2::OBJECT_KIND_WEBSITE
                                    : flexedge::node::v2::OBJECT_KIND_CERTIFICATE);
            reference->set_digest_sha256(object.digest_sha256());
        }
        active.mutable_node_spec()->set_digest_sha256(
            flexedge::node::artifactDigest(active.node_spec().content()));
        active.mutable_release()->set_digest_sha256(flexedge::node::artifactDigest(*manifest));
    }
};

TestConfig snapshot(bool forceHttps = true) {
    TestConfig value;
    auto* nodeSpec = value.active.mutable_node_spec()->mutable_content();
    nodeSpec->set_node_id("f3384ad8-afbe-4aa4-8fe5-b292809c4e04");
    nodeSpec->set_schema_version(flexedge::node::kNodeSpecSchemaVersion);
    nodeSpec->set_revision(3);
    nodeSpec->set_enabled(true);
    auto* endpoint = nodeSpec->add_endpoints();
    endpoint->set_id("endpoint-1");
    endpoint->set_ip_address("127.0.0.1");
    endpoint->set_http_port(80);
    endpoint->set_https_port(443);
    auto* release = value.active.mutable_release()->mutable_content();
    release->set_cluster_id("53a74853-e03f-4ec9-9560-a9acdf4fe780");
    release->set_release_id("release-3");
    release->set_generation(3);
    release->set_access_domain("edge.example.com");
    release->set_enabled(true);
    release->set_schema_version(flexedge::node::kClusterReleaseSchemaVersion);
    flexedge::node::v2::DeliveryObject websiteObject;
    auto* website = websiteObject.mutable_content()->mutable_website();
    website->set_id("website-1");
    website->set_enabled(true);
    website->set_revision(2);
    website->set_https_enabled(true);
    website->set_force_https(forceHttps);
    website->set_minimum_tls_version("1.2");
    website->set_origin_connect_timeout_seconds(10);
    website->set_origin_read_timeout_seconds(30);
    website->set_health_check_path("/");
    website->set_access_log_enabled(true);
    website->set_access_log_request_headers(true);
    website->set_access_log_request_body(true);
    website->set_access_log_response_headers(true);
    website->add_access_log_status_code_ranges("2xx");
    website->set_health_check_interval_seconds(10);
    website->set_health_check_timeout_seconds(3);
    website->set_health_check_expected_status(200);
    website->set_healthy_threshold(2);
    website->set_unhealthy_threshold(3);
    website->set_response_compression_enabled(true);
    website->set_response_compression_min_bytes(1024);
    website->set_response_compression_max_bytes(32 * 1024 * 1024);
    website->add_response_compression_algorithms("zstd");
    website->add_response_compression_algorithms("br");
    website->add_response_compression_algorithms("gzip");
    website->add_response_compression_mime_types("text/*");
    website->add_response_compression_mime_types("application/json");
    website->add_response_compression_extensions(".html");
    website->add_response_compression_extensions(".txt");
    website->add_response_compression_excluded_extensions(".apk");
    auto* domain = website->add_domains();
    domain->set_hostname("WWW.Example.COM.");
    domain->set_https_enabled(true);
    auto* httpOnlyDomain = website->add_domains();
    httpOnlyDomain->set_hostname("http.example.com");
    httpOnlyDomain->set_https_enabled(false);
    auto* origin = website->add_origins();
    origin->set_id("origin-1");
    origin->set_protocol("http");
    origin->set_host("192.0.2.10");
    origin->set_port(8080);
    origin->set_role("primary");
    origin->set_weight(100);
    origin->set_enabled(true);
    flexedge::node::v2::DeliveryObject certificateObject;
    auto* certificate = certificateObject.mutable_content()->mutable_certificate();
    certificate->set_id("certificate-1");
    certificate->set_certificate_chain_pem("certificate");
    certificate->set_private_key_pem("private-key");
    value.objects.push_back(std::move(certificateObject));
    value.objects.push_back(std::move(websiteObject));
    value.finalize();
    return value;
}

std::shared_ptr<const flexedge::node::CompiledConfig> compiled(TestConfig value) {
    value.finalize();
    return std::make_shared<const flexedge::node::CompiledConfig>(
        std::make_shared<const flexedge::node::v2::ActiveState>(std::move(value.active)),
        std::move(value.objects));
}

void apply(flexedge::node::RuntimeState& runtime, TestConfig value) {
    value.finalize();
    runtime.apply(std::make_shared<const flexedge::node::v2::ActiveState>(std::move(value.active)),
                  std::move(value.objects));
}

std::string request(std::uint16_t port, std::string_view host = "WWW.Example.COM:80",
                    std::string_view extraHeaders = {}) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    const auto bytes = "GET /a?q=1 HTTP/1.1\r\nHost: " + std::string(host) + "\r\n" +
                       std::string(extraHeaders) + "Connection: close\r\n\r\n";
    asio::write(socket, asio::buffer(bytes));
    std::string response;
    std::array<char, 1024> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (error == asio::error::eof) {
            break;
        }
        if (error) {
            throw std::system_error(error, "could not read edge response");
        }
    }
    return response;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

struct PemIdentity final {
    std::string certificate;
    std::string privateKey;
};

PemIdentity makeIdentity(std::string_view commonName, std::int64_t serial) {
    const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048), &EVP_PKEY_free);
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), &X509_free);
    REQUIRE(key != nullptr);
    REQUIRE(certificate != nullptr);
    REQUIRE(X509_set_version(certificate.get(), 2) == 1);
    REQUIRE(ASN1_INTEGER_set_int64(X509_get_serialNumber(certificate.get()), serial) == 1);
    REQUIRE(X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) != nullptr);
    REQUIRE(X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) != nullptr);
    REQUIRE(X509_set_pubkey(certificate.get(), key.get()) == 1);
    auto* subject = X509_get_subject_name(certificate.get());
    REQUIRE(subject != nullptr);
    REQUIRE(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(commonName.data()),
                                       static_cast<int>(commonName.size()), -1, 0) == 1);
    REQUIRE(X509_set_issuer_name(certificate.get(), subject) == 1);
    REQUIRE(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);

    const auto certificateBio =
        std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    const auto keyBio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    REQUIRE(certificateBio != nullptr);
    REQUIRE(keyBio != nullptr);
    REQUIRE(PEM_write_bio_X509(certificateBio.get(), certificate.get()) == 1);
    REQUIRE(PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                     nullptr) == 1);
    char* certificateBytes{};
    char* keyBytes{};
    const auto certificateSize = BIO_get_mem_data(certificateBio.get(), &certificateBytes);
    const auto keySize = BIO_get_mem_data(keyBio.get(), &keyBytes);
    REQUIRE(certificateSize > 0);
    REQUIRE(keySize > 0);
    return {
        .certificate = std::string(certificateBytes, static_cast<std::size_t>(certificateSize)),
        .privateKey = std::string(keyBytes, static_cast<std::size_t>(keySize)),
    };
}

struct TlsResponse final {
    std::string bytes{};
    std::string peerCommonName;
};

TlsResponse tlsRequest(std::uint16_t port, std::string_view serverName) {
    asio::io_context context;
    asio::ssl::context tlsContext(asio::ssl::context::tls_client);
    tlsContext.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tlsContext);
    REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), std::string(serverName).c_str()) == 1);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const auto peer = std::unique_ptr<X509, decltype(&X509_free)>(
        SSL_get1_peer_certificate(stream.native_handle()), &X509_free);
    REQUIRE(peer != nullptr);
    std::array<char, 256> commonName{};
    const auto commonNameSize =
        X509_NAME_get_text_by_NID(X509_get_subject_name(peer.get()), NID_commonName,
                                  commonName.data(), static_cast<int>(commonName.size()));
    REQUIRE(commonNameSize > 0);
    const auto bytes =
        "GET /tls HTTP/1.1\r\nHost: " + std::string(serverName) + "\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(bytes));
    TlsResponse response{
        .peerCommonName = std::string(commonName.data(), static_cast<std::size_t>(commonNameSize))};
    std::array<char, 1024> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.bytes.append(buffer.data(), size);
        if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
            break;
        }
        if (error) {
            throw std::system_error(error, "could not read HTTPS edge response");
        }
    }
    return response;
}

std::string tlsAlpn(std::uint16_t port, std::string_view serverName, bool offerHttp2) {
    asio::io_context context;
    asio::ssl::context tlsContext(asio::ssl::context::tls_client);
    tlsContext.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tlsContext);
    REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), std::string(serverName).c_str()) == 1);
    static constexpr unsigned char http1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    static constexpr unsigned char adaptive[] = {2,   'h', '2', 8,   'h', 't',
                                                 't', 'p', '/', '1', '.', '1'};
    REQUIRE(SSL_set_alpn_protos(
                stream.native_handle(), offerHttp2 ? adaptive : http1,
                static_cast<unsigned int>(offerHttp2 ? sizeof(adaptive) : sizeof(http1))) == 0);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const unsigned char* selected{};
    unsigned int selectedSize{};
    SSL_get0_alpn_selected(stream.native_handle(), &selected, &selectedSize);
    return std::string(reinterpret_cast<const char*>(selected), selectedSize);
}

TlsResponse tlsHttp2Request(std::uint16_t port, std::string_view serverName,
                            std::string_view target = "/tls") {
    asio::io_context context;
    asio::ssl::context tlsContext(asio::ssl::context::tls_client);
    tlsContext.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tlsContext);
    REQUIRE(SSL_set_tlsext_host_name(stream.native_handle(), std::string(serverName).c_str()) == 1);
    static constexpr unsigned char adaptive[] = {2,   'h', '2', 8,   'h', 't',
                                                 't', 'p', '/', '1', '.', '1'};
    REQUIRE(SSL_set_alpn_protos(stream.native_handle(), adaptive, sizeof(adaptive)) == 0);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    REQUIRE(flexedge::node::negotiatedHttpProtocol(stream.native_handle()) ==
            flexedge::node::HttpWireProtocol::kHttp2);
    auto connection = ruvia::Http2Connection::client();
    const auto submitted = connection.submitRequestHead(
        {.method = "GET", .scheme = "https", .authority = serverName, .target = target});
    REQUIRE(submitted.submitted() != nullptr);
    auto flush = [&] {
        while (!connection.pendingOutput().empty()) {
            const std::string output(connection.pendingOutput());
            asio::write(stream, asio::buffer(output));
            REQUIRE(connection.consumeOutput(output.size()) !=
                    ruvia::Http2OutputConsumeStatus::kOutOfRange);
        }
    };
    flush();
    TlsResponse response;
    std::array<char, 16384> input{};
    for (;;) {
        const auto size = stream.read_some(asio::buffer(input));
        REQUIRE(connection.feed(std::string_view(input.data(), size)) !=
                ruvia::Http2FeedResult::kProtocolFailure);
        while (auto event = connection.nextEvent()) {
            if (const auto* head = event->responseHead()) {
                response.peerCommonName = std::to_string(head->head().status().value());
            } else if (auto* body = event->messageBodyChunk()) {
                response.bytes.append(body->bytes());
                (void)connection.acknowledge(body->takeCredit());
            } else if (event->messageEnd()) {
                flush();
                return response;
            } else if (event->streamClosed()) {
                throw std::runtime_error("HTTP/2 stream closed before END_STREAM");
            }
        }
        flush();
    }
}

std::string webSocketRoundTrip(std::uint16_t port) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    constexpr std::string_view handshake = "GET /ws HTTP/1.1\r\n"
                                           "Host: www.example.com\r\n"
                                           "Upgrade: websocket\r\n"
                                           "Connection: keep-alive, Upgrade\r\n"
                                           "Sec-WebSocket-Version: 13\r\n"
                                           "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    asio::write(socket, asio::buffer(handshake));
    std::string response;
    std::array<char, 1024> buffer{};
    while (!response.contains("\r\n\r\n")) {
        const auto size = socket.read_some(asio::buffer(buffer));
        response.append(buffer.data(), size);
    }
    REQUIRE(response.starts_with("HTTP/1.1 101 Switching Protocols\r\n"));
    constexpr std::string_view clientBytes = "PING";
    asio::write(socket, asio::buffer(clientBytes));
    std::array<char, 4> serverBytes{};
    asio::read(socket, asio::buffer(serverBytes));
    return std::string(serverBytes.data(), serverBytes.size());
}

} // namespace

// This integration test owns one shared runtime and network lifecycle so each assertion observes
// the same state.
// NOLINTNEXTLINE(google-readability-function-size)
int main(int argc, char* argv[]) {
    try {
        if (argc < 1 || argv == nullptr || argv[0] == nullptr) {
            throw std::runtime_error("test executable path is unavailable");
        }
        const auto executablePath = std::filesystem::absolute(argv[0]);
        const auto credentialsRoot =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-credentials-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct CredentialsDirectoryCleanup final {
            std::filesystem::path path;
            ~CredentialsDirectoryCleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } credentialsCleanup{credentialsRoot};
        const auto credentialsPath = credentialsRoot / "credentials";
        constexpr std::string_view nodeId = "0123456789abcdef0123456789abcdef";
        constexpr std::string_view secret = "0123456789abcdefABCDEFghijklmnop";
        flexedge::node::writeSecureFileAtomic(credentialsPath,
                                              "node_id=0123456789abcdef0123456789abcdef\n"
                                              "secret=0123456789abcdefABCDEFghijklmnop\n");
        auto credentials = flexedge::node::NodeCredentials::load(credentialsPath);
        REQUIRE(credentials.nodeId() == nodeId);
        REQUIRE(credentials.secret() == secret);
        REQUIRE(std::filesystem::exists(credentialsPath));
#ifndef _WIN32
        const auto credentialPermissions = std::filesystem::status(credentialsPath).permissions();
        REQUIRE((credentialPermissions & std::filesystem::perms::group_all) ==
                std::filesystem::perms::none);
        REQUIRE((credentialPermissions & std::filesystem::perms::others_all) ==
                std::filesystem::perms::none);
#endif

        constexpr std::string_view releaseDigest =
            "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac";
        const auto release = flexedge::node::parseNodeReleaseHeaders(
            "HTTP/2 200\r\n"
            "etag: \"76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\"\r\n"
            "X-FlexEdge-Node-SHA256: "
            "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\r\n"
            "x-flexedge-node-version: 0.3.3\r\n\r\n");
        REQUIRE(release);
        REQUIRE(release->statusCode == 200);
        REQUIRE(release->sha256 == releaseDigest);
        REQUIRE(release->version == "0.3.3");
        REQUIRE(release->entityTag == flexedge::node::nodeReleaseEntityTag(releaseDigest));
        const auto releaseHeadersPath = credentialsRoot / "release.headers";
        {
            std::ofstream output(releaseHeadersPath, std::ios::binary);
            output
                << "HTTP/2 200\r\n"
                   "ETag: \"76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\"\r\n"
                   "X-FlexEdge-Node-SHA256: "
                   "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\r\n"
                   "X-FlexEdge-Node-Version: 0.3.3\r\n\r\n";
        }
        const auto releaseFromFile = flexedge::node::readNodeReleaseResponse(releaseHeadersPath);
        REQUIRE(releaseFromFile);
        REQUIRE(releaseFromFile->sha256 == releaseDigest);

        const auto unchangedRelease = flexedge::node::parseNodeReleaseHeaders(
            "HTTP/1.1 302 Found\r\nLocation: https://edge.example/node\r\n\r\n"
            "HTTP/2 304\r\n"
            "ETag: \"76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\"\r\n"
            "X-FlexEdge-Node-SHA256: "
            "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\r\n"
            "X-FlexEdge-Node-Version: 0.3.3\r\n\r\n");
        REQUIRE(unchangedRelease);
        REQUIRE(unchangedRelease->statusCode == 304);
        REQUIRE(!flexedge::node::parseNodeReleaseHeaders(
            "HTTP/1.1 200 OK\r\nETag: \"bad\"\r\n"
            "X-FlexEdge-Node-SHA256: "
            "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\r\n"
            "X-FlexEdge-Node-Version: 0.3.3\r\n\r\n"));
        REQUIRE(!flexedge::node::parseNodeReleaseHeaders(
            "HTTP/1.1 200 OK\r\n"
            "ETag: \"76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\"\r\n"
            "X-FlexEdge-Node-SHA256: "
            "76e30167972c658115198c3dd5d851c585fdffc29a0289bd49f1a5663ddabeac\r\n"
            "X-FlexEdge-Node-Version: 0.3.3\r\n"
            "X-FlexEdge-Node-Version: 0.3.3\r\n\r\n"));

        flexedge::node::NodeLogBuffer deliveryBuffer;
        deliveryBuffer.access({
            .websiteId = "89cbd310-6e73-4e47-b020-0863ed440880",
            .clientIp = "192.0.2.1",
            .protocol = "HTTP/1.1",
            .method = "GET",
            .host = "www.example.com",
            .target = "/",
            .statusCode = 200,
            .userAgent = "test-agent",
            .referer = "https://example.com/",
            .requestHeaders = "Host: www.example.com\n",
            .requestBody = "payload",
            .tlsFingerprint = "ja3:0123456789abcdef0123456789abcdef",
            .responseHeaders = "Content-Type: text/plain\n",
            .queryString = "debug=1",
            .cookies = flexedge::node::NodeLogBuffer::cookieValue("sid=abc"),
        });
        deliveryBuffer.node("warning", "control", "retry delivery");
        REQUIRE(deliveryBuffer.pending());
        auto firstDelivery = deliveryBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(firstDelivery && firstDelivery->value().events_size() == 2);
        REQUIRE(firstDelivery->value().events(0).has_access_log());
        REQUIRE(firstDelivery->value().events(1).has_node_log());
        const auto accessLogId = firstDelivery->value().events(0).access_log().id();
        REQUIRE(firstDelivery->value().events(0).access_log().request_headers().contains("Host:"));
        REQUIRE(firstDelivery->value().events(0).access_log().request_body() == "payload");
        REQUIRE(
            firstDelivery->value().events(0).access_log().tls_fingerprint().starts_with("ja3:"));
        REQUIRE(firstDelivery->value().events(0).access_log().response_headers().contains(
            "Content-Type:"));
        REQUIRE(firstDelivery->value().events(0).access_log().query_string() == "debug=1");
        REQUIRE(firstDelivery->value().events(0).access_log().cookies() == "sid=abc");
        deliveryBuffer.restore(std::move(*firstDelivery));
        auto retriedDelivery = deliveryBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(retriedDelivery && retriedDelivery->value().events_size() == 2);
        REQUIRE(retriedDelivery->value().events(0).access_log().id() == accessLogId);
        REQUIRE(deliveryBuffer.pending());
        deliveryBuffer.acknowledge(std::move(*retriedDelivery));
        REQUIRE(flexedge::node::NodeLogBuffer::requestHeaderValue("Authorization", "secret") ==
                "secret");
        REQUIRE(flexedge::node::NodeLogBuffer::requestBodyValue("{\"token\":\"secret\"}") ==
                "{\"token\":\"secret\"}");
        REQUIRE(flexedge::node::NodeLogBuffer::cookieValue("sid=abc; theme=dark") ==
                "sid=abc; theme=dark");

        flexedge::node::v2::Website websiteLogPolicy;
        REQUIRE(!websiteLogPolicy.access_log_enabled());
        websiteLogPolicy.set_access_log_enabled(true);
        REQUIRE(websiteLogPolicy.access_log_enabled());

        flexedge::node::NodeLogBuffer highVolumeBuffer;
        for (std::size_t index = 0; index < 600; ++index) {
            highVolumeBuffer.node("info", "load", "event-" + std::to_string(index));
        }
        auto highVolumeFirst = highVolumeBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(highVolumeFirst && highVolumeFirst->value().events_size() == 512);
        REQUIRE(highVolumeFirst->value().ByteSizeLong() <= 1024 * 1024);
        highVolumeBuffer.acknowledge(std::move(*highVolumeFirst));
        auto highVolumeSecond = highVolumeBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(highVolumeSecond && highVolumeSecond->value().events_size() == 88);
        highVolumeBuffer.acknowledge(std::move(*highVolumeSecond));
        REQUIRE(!highVolumeBuffer.pending());

        flexedge::node::NodeLogBuffer concurrentBuffer(4);
        std::vector<std::thread> producers;
        for (std::size_t worker = 0; worker < 4; ++worker) {
            const auto producer = concurrentBuffer.workerProducer(worker);
            producers.emplace_back([producer, worker] {
                for (std::size_t index = 0; index < 2000; ++index) {
                    producer.node("info", "load",
                                  "worker-" + std::to_string(worker) + '-' + std::to_string(index));
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
        REQUIRE(concurrentBuffer.queuedEvents() == 8000);
        REQUIRE(concurrentBuffer.droppedEvents() == 0);
        for (std::size_t index = 0; index < 512; ++index) {
            concurrentBuffer.node("info", "load", "overflow-" + std::to_string(index));
        }
        REQUIRE(concurrentBuffer.queuedEvents() == 8512);
        REQUIRE(concurrentBuffer.droppedEvents() == 0);
        auto retainedDelivery = concurrentBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(retainedDelivery && retainedDelivery->value().events_size() == 512);
        REQUIRE(!concurrentBuffer.node("info", "load", "must-not-reuse-in-flight-capacity"));
        concurrentBuffer.restore(std::move(*retainedDelivery));
        REQUIRE(concurrentBuffer.queuedEvents() == 8512);
        std::size_t deliveredEvents{};
        while (auto delivery = concurrentBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d")) {
            REQUIRE(delivery->value().ByteSizeLong() <= 1024 * 1024);
            deliveredEvents += static_cast<std::size_t>(delivery->value().events_size());
            concurrentBuffer.acknowledge(std::move(*delivery));
        }
        REQUIRE(deliveredEvents == 8512);
        REQUIRE(concurrentBuffer.droppedEvents() == 1);

        flexedge::node::NodeLogBuffer interleavedBuffer(1);
        const auto interleavedProducer = interleavedBuffer.workerProducer(0);
        std::atomic_bool interleavedProducerDone{};
        std::atomic<std::size_t> interleavedAccepted{};
        std::atomic<std::size_t> interleavedDelivered{};
        std::thread interleavedConsumer([&] {
            while (!interleavedProducerDone.load(std::memory_order_acquire) ||
                   interleavedBuffer.pending()) {
                if (auto delivery =
                        interleavedBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d")) {
                    interleavedDelivered.fetch_add(
                        static_cast<std::size_t>(delivery->value().events_size()),
                        std::memory_order_relaxed);
                    interleavedBuffer.acknowledge(std::move(*delivery));
                } else {
                    std::this_thread::yield();
                }
            }
        });
        std::thread interleavedWriter([&] {
            for (std::size_t index = 0; index < 4096; ++index) {
                if (interleavedProducer.node("info", "load",
                                             "interleaved-" + std::to_string(index))) {
                    interleavedAccepted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            interleavedProducerDone.store(true, std::memory_order_release);
        });
        interleavedWriter.join();
        interleavedConsumer.join();
        REQUIRE(interleavedAccepted == 4096);
        REQUIRE(interleavedDelivered == interleavedAccepted.load());
        REQUIRE(interleavedBuffer.droppedEvents() == 0);
        REQUIRE(!interleavedBuffer.pending());

        flexedge::node::NodeLogBuffer controlDrainBuffer(1);
        const auto backgroundLogs = controlDrainBuffer.backgroundProducer();
        REQUIRE(backgroundLogs.node("info", "upgrade", "upgrade completed"));
        REQUIRE(!controlDrainBuffer.waitForControlDrain(std::chrono::milliseconds(1)));
        auto controlDelivery = controlDrainBuffer.take("576369c6-946e-4dee-8434-9b8909d72dbd5");
        REQUIRE(controlDelivery.has_value());
        REQUIRE(!controlDrainBuffer.waitForDrain(std::chrono::milliseconds(1)));
        controlDrainBuffer.acknowledge(std::move(*controlDelivery));
        REQUIRE(controlDrainBuffer.waitForControlDrain(std::chrono::milliseconds(1)));
        REQUIRE(controlDrainBuffer.waitForDrain(std::chrono::milliseconds(1)));

        flexedge::node::NodeLogBuffer priorityBuffer(4);
        const auto firstPriorityProducer = priorityBuffer.workerProducer(0);
        const auto secondPriorityProducer = priorityBuffer.workerProducer(1);
        for (std::size_t index = 0; index < 512; ++index) {
            REQUIRE(firstPriorityProducer.node("info", "access", "first-" + std::to_string(index)));
            REQUIRE(
                secondPriorityProducer.node("info", "access", "second-" + std::to_string(index)));
        }
        auto firstPriorityDelivery = priorityBuffer.take("576369c6-946e-4dee-8434-9b8909d72dbd5");
        REQUIRE(firstPriorityDelivery && firstPriorityDelivery->value().events_size() == 512);
        priorityBuffer.acknowledge(std::move(*firstPriorityDelivery));
        const auto priorityLogs = priorityBuffer.backgroundProducer();
        REQUIRE(priorityLogs.node("info", "upgrade", "upgrade completed"));
        auto priorityDelivery = priorityBuffer.take("576369c6-946e-4dee-8434-9b8909d72dbd5");
        REQUIRE(priorityDelivery && priorityDelivery->value().events_size() == 512);
        REQUIRE(priorityDelivery->value().events(0).node_log().category() == "upgrade");
        priorityBuffer.acknowledge(std::move(*priorityDelivery));

        flexedge::node::SelfUpdater notifiedUpdater({
            .serverOrigin = "http://127.0.0.1",
            .binaryPath = executablePath,
            .currentVersion = "0.0.0",
            .upgradeRecordPath = credentialsRoot / "pending-upgrade",
            .nodeLog = {},
            .requestRestart = {},
        });
        REQUIRE(!notifiedUpdater.notifyRelease("invalid"));
        REQUIRE(!notifiedUpdater.notifyRelease(flexedge::node::binarySha256(executablePath)));
        REQUIRE(notifiedUpdater.notifyRelease(
            "a1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea"));

        flexedge::node::NodeLogBuffer boundedBodyBuffer;
        boundedBodyBuffer.access({
            .websiteId = "89cbd310-6e73-4e47-b020-0863ed440880",
            .clientIp = "192.0.2.1",
            .protocol = "HTTP/1.1",
            .method = "POST",
            .host = "www.example.com",
            .target = "/upload",
            .statusCode = 200,
            .userAgent = {},
            .referer = {},
            .requestHeaders = {},
            .requestBody = std::string(2 * 1024 * 1024, 'x'),
            .tlsFingerprint = {},
            .responseHeaders = {},
            .queryString = {},
            .cookies = {},
        });
        auto boundedBodyDelivery = boundedBodyBuffer.take("cd45a676-4a24-44b5-8974-ceba6d7cc94d");
        REQUIRE(boundedBodyDelivery && boundedBodyDelivery->value().events_size() == 1);
        const auto& boundedAccess = boundedBodyDelivery->value().events(0).access_log();
        REQUIRE(boundedAccess.request_body().size() ==
                flexedge::node::NodeLogBuffer::kMaxRequestBodyBytes);
        REQUIRE(boundedAccess.request_body_truncated());
        REQUIRE(boundedBodyDelivery->value().ByteSizeLong() <= 1024 * 1024);
        boundedBodyBuffer.acknowledge(std::move(*boundedBodyDelivery));

        const auto chunked = flexedge::node::decodeChunkedBody(
            "4\r\nWiki\r\n5;name=value\r\npedia\r\n0\r\nX-Test: value\r\n\r\n", 64);
        REQUIRE(chunked && *chunked == "Wikipedia");
        REQUIRE(!flexedge::node::decodeChunkedBody("4\r\nWiki\r\n", 64));
        REQUIRE(!flexedge::node::decodeChunkedBody("5\r\nhello\r\n0\r\n\r\n", 4));
        REQUIRE(flexedge::node::acceptsEventStream("text/event-stream"));
        REQUIRE(flexedge::node::acceptsEventStream(
            "application/json, text/event-stream; charset=utf-8"));
        REQUIRE(!flexedge::node::acceptsEventStream("text/plain"));
        const std::array<ruvia::HttpHeaderView, 2> streamingHeaders{
            ruvia::HttpHeaderView{"Accept", "text/event-stream"},
            ruvia::HttpHeaderView{"Accept-Encoding", "gzip"},
        };
        const auto streamingConfig = snapshot();
        const auto streamingOriginRequest = flexedge::node::prepareOriginRequest(
            {.method = "GET",
             .target = "/events",
             .headers = streamingHeaders,
             .body = {},
             .hasBody = false},
            streamingConfig.website(), "www.example.com", "198.51.100.1", true, false, false, true);
        REQUIRE(streamingOriginRequest);
        REQUIRE(streamingOriginRequest->bytes.contains("Accept-Encoding: identity\r\n"));
        REQUIRE(!streamingOriginRequest->bytes.contains("Accept-Encoding: gzip\r\n"));
        flexedge::node::ChunkedWireTracker chunkedTracker;
        REQUIRE(chunkedTracker.consume("4\r\nWi") ==
                flexedge::node::ChunkedWireStatus::kIncomplete);
        REQUIRE(chunkedTracker.consume("ki\r\n0\r\nX-Test: value\r\n") ==
                flexedge::node::ChunkedWireStatus::kIncomplete);
        REQUIRE(chunkedTracker.consume("\r\n") == flexedge::node::ChunkedWireStatus::kComplete);
        REQUIRE(chunkedTracker.consume("extra") == flexedge::node::ChunkedWireStatus::kInvalid);

        const std::string bufferedResponse = "HTTP/1.1 100 Continue\r\nX-Ignored: yes\r\n\r\n"
                                             "HTTP/1.1 200 OK\r\n"
                                             "Connection: keep-alive, X-Remove\r\n"
                                             "X-Remove: secret\r\n"
                                             "Transfer-Encoding: chunked\r\n"
                                             "X-Test: value \r\n\r\n"
                                             "4\r\nbody\r\n0\r\n\r\n";
        const auto bufferedHead =
            flexedge::node::detail::parseBufferedResponseHead(bufferedResponse);
        REQUIRE(bufferedHead);
        REQUIRE(bufferedHead->status == 200);
        REQUIRE(bufferedHead->chunked);
        REQUIRE(bufferedHead->bodyOffset == bufferedResponse.find("4\r\nbody"));
        REQUIRE(bufferedHead->headers.size() == 1);
        REQUIRE(bufferedHead->headers.front().first == "X-Test");
        REQUIRE(bufferedHead->headers.front().second == "value ");
        REQUIRE(
            !flexedge::node::detail::parseBufferedResponseHead("HTTP/1.1 20x Bad Status\r\n\r\n"));

        flexedge::node::BufferedBytesBudget budget(8192);
        auto firstReservation = budget.lease();
        REQUIRE(firstReservation.tryGrow(4096));
        REQUIRE(budget.usedBytes() == 4096);
        auto movedReservation = std::move(firstReservation);
        movedReservation.shrink(1024);
        REQUIRE(budget.usedBytes() == 3072);
        movedReservation = {};
        REQUIRE(budget.usedBytes() == 0);
        auto maximumReservation = budget.lease();
        REQUIRE(maximumReservation.tryGrow(budget.maximumBytes()));
        auto rejectedReservation = budget.lease();
        REQUIRE(!rejectedReservation.tryGrow(1));
        REQUIRE(budget.rejectedReservations() == 1);
        maximumReservation = {};
        REQUIRE(budget.usedBytes() == 0);

        flexedge::node::BufferedBytesBudget requestBudget(32);
        auto heldRequestBytes = requestBudget.lease();
        REQUIRE(heldRequestBytes.tryGrow(32));
        auto rejectedRequestBytes = requestBudget.lease();
        REQUIRE(!rejectedRequestBytes.tryGrow(1));
        REQUIRE(requestBudget.rejectedReservations() == 1);

        flexedge::node::BufferedProxyRequest compressionRequest;
        compressionRequest.headers.emplace_back("Accept-Encoding", "gzip;q=0.5, br, zstd;q=0.8");
        flexedge::node::BufferedProxyResponse compressionResponse;
        compressionResponse.status = 200;
        compressionResponse.headers.emplace_back("Content-Type", "text/plain; charset=utf-8");
        compressionResponse.body.assign(4096, 'a');
        flexedge::node::BufferedBytesBudget compressionBudget(
            flexedge::node::kMaximumResponseBufferBytes);
        compressionResponse.reservation = compressionBudget.lease();
        REQUIRE(compressionResponse.reservation.tryGrow(compressionResponse.body.size()));
        flexedge::node::v2::Website compressionWebsite;
        compressionWebsite.set_response_compression_enabled(true);
        compressionWebsite.set_response_compression_min_bytes(256);
        compressionWebsite.set_response_compression_max_bytes(32 * 1024 * 1024);
        compressionWebsite.add_response_compression_algorithms("br");
        compressionWebsite.add_response_compression_mime_types("text/*");
        compressionWebsite.add_response_compression_extensions(".txt");
        compressionWebsite.add_response_compression_excluded_extensions(".apk");
        flexedge::node::applyResponseCompression(compressionRequest, compressionWebsite,
                                                 compressionResponse);
        REQUIRE(flexedge::node::responseHeader(compressionResponse, "Content-Encoding") == "br");
        REQUIRE(flexedge::node::responseHeader(compressionResponse, "Vary") == "Accept-Encoding");
        auto decodedCompression = ruvia::decodeHttpContent(
            ruvia::HttpContentCoding::kBrotli, compressionResponse.body, {.maxDecodedBytes = 4096});
        REQUIRE(decodedCompression.decoded() != nullptr);
        REQUIRE(decodedCompression.decoded()->bytes() == std::string(4096, 'a'));
        REQUIRE(compressionBudget.usedBytes() == 4096);

        auto plainCompressionResponse = [] {
            flexedge::node::BufferedProxyResponse response;
            response.status = 200;
            response.headers.emplace_back("Content-Type", "text/plain");
            response.body.assign(4096, 'a');
            return response;
        };
        auto excludedCompressionRequest = compressionRequest;
        excludedCompressionRequest.target = "/download/package.APK?token=1";
        auto excludedCompressionResponse = plainCompressionResponse();
        flexedge::node::applyResponseCompression(excludedCompressionRequest, compressionWebsite,
                                                 excludedCompressionResponse);
        REQUIRE(flexedge::node::responseHeader(excludedCompressionResponse, "Content-Encoding")
                    .empty());

        auto partialCompressionResponse = plainCompressionResponse();
        partialCompressionResponse.status = 206;
        flexedge::node::applyResponseCompression(compressionRequest, compressionWebsite,
                                                 partialCompressionResponse);
        REQUIRE(
            flexedge::node::responseHeader(partialCompressionResponse, "Content-Encoding").empty());

        auto limitedCompressionWebsite = compressionWebsite;
        limitedCompressionWebsite.set_response_compression_max_bytes(1024);
        auto limitedCompressionResponse = plainCompressionResponse();
        flexedge::node::applyResponseCompression(compressionRequest, limitedCompressionWebsite,
                                                 limitedCompressionResponse);
        REQUIRE(
            flexedge::node::responseHeader(limitedCompressionResponse, "Content-Encoding").empty());

        flexedge::node::BufferedBytesBudget exhaustedCompressionBudget(
            flexedge::node::kMaximumResponseBufferBytes);
        auto heldCompressionBudget = exhaustedCompressionBudget.lease();
        REQUIRE(heldCompressionBudget.tryGrow(exhaustedCompressionBudget.maximumBytes()));
        auto overloadedCompressionResponse = plainCompressionResponse();
        overloadedCompressionResponse.reservation = exhaustedCompressionBudget.lease();
        flexedge::node::applyResponseCompression(compressionRequest, compressionWebsite,
                                                 overloadedCompressionResponse);
        REQUIRE(flexedge::node::responseHeader(overloadedCompressionResponse, "Content-Encoding")
                    .empty());

        auto originEncoded = ruvia::encodeHttpContent(
            ruvia::HttpContentCoding::kGzip, std::string(4096, 'a'), {.maxEncodedBytes = 4096});
        REQUIRE(originEncoded.encoded() != nullptr);
        auto recompressedResponse = plainCompressionResponse();
        recompressedResponse.body.assign(originEncoded.encoded()->bytes());
        recompressedResponse.headers.emplace_back("Content-Encoding", "gzip");
        const auto originalEncodedBody = recompressedResponse.body;
        flexedge::node::applyResponseCompression(compressionRequest, compressionWebsite,
                                                 recompressedResponse);
        REQUIRE(recompressedResponse.body == originalEncodedBody);

        flexedge::node::StateCipher stateCipher(std::string(64, '1'));
        const auto sealedState = stateCipher.seal("device-encrypted-private-state");
        REQUIRE(sealedState.starts_with("FES1"));
        REQUIRE(!sealedState.contains("device-encrypted-private-state"));
        REQUIRE(stateCipher.open(sealedState) == "device-encrypted-private-state");
        auto tamperedState = sealedState;
        tamperedState.back() ^= 1;
        bool tamperedStateRejected = false;
        try {
            (void)stateCipher.open(tamperedState);
        } catch (const std::runtime_error&) {
            tamperedStateRejected = true;
        }
        REQUIRE(tamperedStateRejected);

        const auto stateDirectory =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-state-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct StateDirectoryCleanup final {
            std::filesystem::path path;
            ~StateDirectoryCleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } stateCleanup{stateDirectory};
        const auto nodeDigest = flexedge::node::binarySha256(executablePath);
        const auto upgradeRecordPath = stateDirectory / "pending-upgrade";
        const flexedge::node::UpgradeRecord upgradeRecord{
            .previousVersion = "0.3.23",
            .currentVersion = "0.3.24",
            .targetSha256 = nodeDigest,
        };
        flexedge::node::writePendingUpgradeRecord(upgradeRecordPath, upgradeRecord);
        const auto restoredUpgrade =
            flexedge::node::takePendingUpgradeRecord(upgradeRecordPath, nodeDigest);
        REQUIRE(restoredUpgrade.has_value());
        REQUIRE(restoredUpgrade->message() == upgradeRecord.message());
        REQUIRE(!std::filesystem::exists(upgradeRecordPath));
        flexedge::node::writePendingUpgradeRecord(upgradeRecordPath, upgradeRecord);
        REQUIRE(!flexedge::node::takePendingUpgradeRecord(
            upgradeRecordPath, "e1408004be0e7e8736f9365f835cb1454adeeda979a0b08bc60acf753aa2e4ea"));
        REQUIRE(!std::filesystem::exists(upgradeRecordPath));
        flexedge::node::StateStore stateStore(stateDirectory,
                                              "3df64f75-2df7-43b8-aa89-661920126564");
        auto storedSnapshot = snapshot();
        storedSnapshot.finalize();
        stateStore.stage(storedSnapshot.active, storedSnapshot.objects);
        stateStore.activateStaged();
        const auto persisted = stateStore.load();
        REQUIRE(persisted.active.SerializeAsString() == storedSnapshot.active.SerializeAsString());
        REQUIRE(persisted.objects.size() == storedSnapshot.objects.size());
        auto staleSnapshot = snapshot();
        staleSnapshot.active.mutable_node_spec()->mutable_content()->set_schema_version(1);
        staleSnapshot.finalize();
        stateStore.stage(staleSnapshot.active, staleSnapshot.objects);
        stateStore.activateStaged();
        const auto discarded = stateStore.load();
        REQUIRE(!discarded.active.has_node_spec());
        REQUIRE(discarded.objects.empty());
        auto staleRelease = snapshot();
        staleRelease.active.mutable_release()->mutable_content()->set_schema_version(1);
        staleRelease.finalize();
        stateStore.stage(staleRelease.active, staleRelease.objects);
        stateStore.activateStaged();
        const auto discardedRelease = stateStore.load();
        REQUIRE(!discardedRelease.active.has_release());
        REQUIRE(discardedRelease.objects.empty());

        flexedge::node::OriginHealthRegistry healthState;
        REQUIRE(healthState.healthy("website", "origin"));
        healthState.failure("website", "origin", 2);
        REQUIRE(healthState.healthy("website", "origin"));
        healthState.failure("website", "origin", 2);
        REQUIRE(!healthState.healthy("website", "origin"));
        healthState.success("website", "origin", 2);
        REQUIRE(!healthState.healthy("website", "origin"));
        healthState.success("website", "origin", 2);
        REQUIRE(healthState.healthy("website", "origin"));
        flexedge::node::OriginHealthRegistry::KeySet retainedHealthKeys;
        retainedHealthKeys.emplace(flexedge::node::OriginHealthRegistry::key("website", "origin"));
        healthState.retain(retainedHealthKeys);
        REQUIRE(healthState.healthy("website", "origin"));
        healthState.failure("website", "origin", 1);
        std::vector<std::thread> healthReporters;
        for (std::size_t index = 0; index < 8; ++index) {
            healthReporters.emplace_back([&] { healthState.success("website", "origin", 1); });
        }
        for (auto& reporter : healthReporters) {
            reporter.join();
        }
        REQUIRE(healthState.healthy("website", "origin"));
        retainedHealthKeys.clear();
        healthState.retain(retainedHealthKeys);
        REQUIRE(healthState.healthy("website", "origin"));

        retainedHealthKeys.emplace(flexedge::node::OriginHealthRegistry::key("website", "origin"));
        std::atomic<bool> observeHealth{true};
        std::atomic<std::uint64_t> healthObservations{};
        std::vector<std::thread> healthReaders;
        for (std::size_t index = 0; index < 8; ++index) {
            healthReaders.emplace_back([&] {
                while (observeHealth.load(std::memory_order_relaxed)) {
                    if (healthState.healthy("website", "origin")) {
                        healthObservations.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (std::size_t index = 0; index < 64; ++index) {
            healthState.retain(retainedHealthKeys);
            healthState.success("website", "origin", 1);
        }
        observeHealth.store(false, std::memory_order_relaxed);
        for (auto& reader : healthReaders) {
            reader.join();
        }
        REQUIRE(healthObservations.load(std::memory_order_relaxed) != 0);
        REQUIRE(healthState.healthy("website", "origin"));

        const auto config = compiled(snapshot());
        REQUIRE(config->website("www.example.com:80") != nullptr);
        REQUIRE(config->domain("WWW.EXAMPLE.COM") != nullptr);
        const auto redirect =
            flexedge::node::httpToHttpsRedirect(*config, "WWW.Example.COM:80", "/a?q=1");
        REQUIRE(redirect);
        REQUIRE(redirect->status == ruvia::http_status::kMovedPermanently);
        REQUIRE(redirect->location == "https://www.example.com/a?q=1");
        REQUIRE(!flexedge::node::httpToHttpsRedirect(*config, "http.example.com", "/"));

        const auto noRedirect = compiled(snapshot(false));
        REQUIRE(!flexedge::node::httpToHttpsRedirect(*noRedirect, "www.example.com", "/"));

        auto supportedHttp2 = snapshot(false);
        supportedHttp2.mutableWebsite()->set_http2_enabled(true);
        const auto http2Config = compiled(std::move(supportedHttp2));
        REQUIRE(http2Config->website("www.example.com")->http2_enabled());

        auto invalidCompressionMimeType = snapshot(false);
        invalidCompressionMimeType.mutableWebsite()->add_response_compression_mime_types(
            "application");
        bool invalidCompressionMimeTypeRejected = false;
        try {
            (void)compiled(std::move(invalidCompressionMimeType));
        } catch (const std::runtime_error&) {
            invalidCompressionMimeTypeRejected = true;
        }
        REQUIRE(invalidCompressionMimeTypeRejected);

        flexedge::node::RuntimeState runtime;
        apply(runtime, snapshot());
        const auto previous = runtime.config();
        REQUIRE(previous != nullptr);
        REQUIRE(previous->nodeSpec().content().revision() == 3);

        auto duplicate = snapshot();
        duplicate.setGeneration(4);
        duplicate.duplicateWebsite();
        bool rejected = false;
        try {
            apply(runtime, std::move(duplicate));
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        REQUIRE(rejected);
        REQUIRE(runtime.config() == previous);
        REQUIRE(runtime.appliedNodeSpecRevision() == 3);

        auto replacement = snapshot(false);
        replacement.setGeneration(4);
        apply(runtime, std::move(replacement));
        REQUIRE(runtime.config() != previous);
        REQUIRE(runtime.appliedNodeSpecRevision() == 4);
        REQUIRE(previous->website("www.example.com") != nullptr);

        ruvia::EventLoopPool loops({.loopCount = 2});
        flexedge::node::OriginHealthRegistry listenerHealth;
        flexedge::node::OriginTlsContext firstOriginTls;
        flexedge::node::OriginTlsContext secondOriginTls;
        flexedge::node::OriginConnectionPool firstOriginConnections(loops.loop(0).executor(),
                                                                    firstOriginTls);
        flexedge::node::OriginConnectionPool secondOriginConnections(loops.loop(1).executor(),
                                                                     secondOriginTls);
        flexedge::node::RuntimeMetrics runtimeMetrics;
        flexedge::node::BufferedBytesBudget requestBuffers(
            flexedge::node::kMaximumRequestBufferBytes);
        flexedge::node::BufferedBytesBudget responseBuffers(
            flexedge::node::kMaximumResponseBufferBytes);
        flexedge::node::NodeLogBuffer logBuffer(2);
        auto firstWorkerLogs = logBuffer.workerProducer(0);
        auto secondWorkerLogs = logBuffer.workerProducer(1);
        (void)runtimeMetrics.sample();
        auto firstListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(0), runtime, listenerHealth, runtimeMetrics, firstWorkerLogs,
            firstOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto port = firstListener->localEndpoint().port();
        auto secondListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(1), runtime, listenerHealth, runtimeMetrics, secondWorkerLogs,
            secondOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
        flexedge::node::BufferedBytesBudget constrainedRequestBuffers(32);
        auto constrainedListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(0), runtime, listenerHealth, runtimeMetrics, firstWorkerLogs,
            firstOriginConnections, constrainedRequestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto constrainedPort = constrainedListener->localEndpoint().port();
        firstListener->start();
        secondListener->start();
        constrainedListener->start();
        loops.start();
        const auto constrainedResponse = request(constrainedPort);
        REQUIRE(constrainedResponse.starts_with("HTTP/1.1 503 Service Unavailable\r\n"));
        REQUIRE(waitUntil([&] { return constrainedRequestBuffers.usedBytes() == 0; },
                          std::chrono::seconds(1)));
        REQUIRE(constrainedRequestBuffers.rejectedReservations() == 1);
        constrainedListener->stop();
        const auto noRouteResponse = request(port, "unknown.example.com");
        REQUIRE(noRouteResponse.starts_with("HTTP/1.1 421 Misdirected Request\r\n"));
        REQUIRE(waitUntil([&] { return runtimeMetrics.connectionCount() == 0; },
                          std::chrono::seconds(1)));
        const auto observedMetrics = runtimeMetrics.sample();
        REQUIRE(observedMetrics.trafficOutBps > 0);
        REQUIRE(observedMetrics.connectionCount == 0);
        REQUIRE(observedMetrics.cpuUsage >= 0 && observedMetrics.cpuUsage <= 1);
        REQUIRE(observedMetrics.memoryUsage >= 0 && observedMetrics.memoryUsage <= 1);

        auto redirectSnapshot = snapshot(true);
        redirectSnapshot.setGeneration(5);
        apply(runtime, std::move(redirectSnapshot));
        const auto redirectResponse = request(port);
        REQUIRE(redirectResponse.starts_with("HTTP/1.1 301 Moved Permanently\r\n"));
        REQUIRE(redirectResponse.contains("Location: https://www.example.com/a?q=1\r\n"));

        auto routeSnapshot = snapshot(false);
        routeSnapshot.setGeneration(6);
        auto* route = routeSnapshot.mutableWebsite()->add_route_rules();
        route->set_id("route-1");
        route->set_enabled(true);
        route->set_match_type("prefix");
        route->set_path("/a");
        route->add_methods("GET");
        route->set_action("redirect");
        route->set_redirect_url("/moved");
        route->set_redirect_status(302);
        auto* routeHeader = route->add_response_headers();
        routeHeader->set_name("X-Edge-Route");
        routeHeader->set_value("matched");
        apply(runtime, std::move(routeSnapshot));
        const auto routeResponse = request(port);
        REQUIRE(routeResponse.starts_with("HTTP/1.1 302 Found\r\n"));
        REQUIRE(routeResponse.contains("Location: /moved?q=1\r\n"));
        REQUIRE(routeResponse.contains("X-Edge-Route: matched\r\n"));

        asio::io_context originContext;
        asio::ip::tcp::acceptor originAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto originPort = originAcceptor.local_endpoint().port();
        asio::ip::tcp::acceptor unavailableAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto unavailablePort = unavailableAcceptor.local_endpoint().port();
        unavailableAcceptor.close();
        std::jthread originServer([&] {
            asio::ip::tcp::socket originSocket(originContext);
            originAcceptor.accept(originSocket);
            std::array<char, 4096> originRequest{};
            std::error_code ignored;
            (void)originSocket.read_some(asio::buffer(originRequest), ignored);
            constexpr std::string_view originResponse =
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: Foo\r\n"
                "Foo: remove-me\r\nX-Origin: retained\r\n\r\nOK";
            asio::write(originSocket, asio::buffer(originResponse));
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        });
        auto proxySnapshot = snapshot(false);
        proxySnapshot.setGeneration(7);
        proxySnapshot.mutableWebsite()->mutable_origins(0)->set_host("127.0.0.1");
        proxySnapshot.mutableWebsite()->mutable_origins(0)->set_port(unavailablePort);
        auto* backup = proxySnapshot.mutableWebsite()->add_origins();
        backup->set_id("origin-backup");
        backup->set_protocol("http");
        backup->set_host("127.0.0.1");
        backup->set_port(originPort);
        backup->set_role("backup");
        backup->set_weight(100);
        backup->set_enabled(true);
        auto* proxyRoute = proxySnapshot.mutableWebsite()->add_route_rules();
        proxyRoute->set_id("route-backup");
        proxyRoute->set_enabled(true);
        proxyRoute->set_match_type("prefix");
        proxyRoute->set_path("/a");
        proxyRoute->set_action("proxy");
        proxyRoute->add_origin_ids("origin-backup");
        const auto candidates =
            flexedge::node::originCandidates(proxySnapshot.website(), 0, proxyRoute);
        REQUIRE(candidates.size() == 1);
        REQUIRE(candidates.front()->id() == "origin-backup");
        apply(runtime, std::move(proxySnapshot));
        const auto proxyStarted = std::chrono::steady_clock::now();
        const auto proxyResponse = request(port);
        const auto proxyElapsed = std::chrono::steady_clock::now() - proxyStarted;
        REQUIRE(proxyResponse.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(proxyResponse.ends_with("\r\n\r\nOK"));
        REQUIRE(proxyResponse.contains("Connection: close\r\n"));
        REQUIRE(proxyResponse.contains("X-Origin: retained\r\n"));
        REQUIRE(!proxyResponse.contains("Foo: remove-me\r\n"));
        REQUIRE(proxyElapsed < std::chrono::seconds(1));

        asio::ip::tcp::acceptor compressionAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto compressionPort = compressionAcceptor.local_endpoint().port();
        std::jthread compressionServer([&] {
            asio::ip::tcp::socket socket(originContext);
            compressionAcceptor.accept(socket);
            std::array<char, 4096> originRequest{};
            std::error_code ignored;
            (void)socket.read_some(asio::buffer(originRequest), ignored);
            const std::string body(4096, 'a');
            const auto response =
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 4096\r\n"
                "Connection: close\r\n\r\n" +
                body;
            asio::write(socket, asio::buffer(response));
        });
        auto compressionSnapshot = snapshot(false);
        compressionSnapshot.setGeneration(7);
        auto* compressionOrigin = compressionSnapshot.mutableWebsite()->mutable_origins(0);
        compressionOrigin->set_host("127.0.0.1");
        compressionOrigin->set_port(compressionPort);
        apply(runtime, std::move(compressionSnapshot));
        const auto compressedProxyResponse =
            request(port, "www.example.com", "Accept-Encoding: gzip\r\n");
        REQUIRE(compressedProxyResponse.contains("Content-Encoding: gzip\r\n"));
        REQUIRE(compressedProxyResponse.contains("Vary: Accept-Encoding\r\n"));
        const auto compressedBodyOffset = compressedProxyResponse.find("\r\n\r\n");
        REQUIRE(compressedBodyOffset != std::string::npos);
        auto decodedProxyResponse = ruvia::decodeHttpContent(
            ruvia::HttpContentCoding::kGzip,
            std::string_view(compressedProxyResponse).substr(compressedBodyOffset + 4),
            {.maxDecodedBytes = 4096});
        REQUIRE(decodedProxyResponse.decoded() != nullptr);
        REQUIRE(decodedProxyResponse.decoded()->bytes() == std::string(4096, 'a'));

        asio::ip::tcp::acceptor healthAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto healthPort = healthAcceptor.local_endpoint().port();
        std::atomic<int> healthRequests{};
        std::jthread healthServer([&] {
            for (int requestIndex = 0; requestIndex < 1; ++requestIndex) {
                asio::ip::tcp::socket healthSocket(originContext);
                healthAcceptor.accept(healthSocket);
                std::array<char, 4096> healthRequest{};
                std::error_code ignored;
                (void)healthSocket.read_some(asio::buffer(healthRequest), ignored);
                ++healthRequests;
                constexpr std::string_view healthResponse =
                    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                asio::write(healthSocket, asio::buffer(healthResponse));
            }
        });
        auto healthSnapshot = snapshot(false);
        healthSnapshot.setGeneration(8);
        auto* healthWebsite = healthSnapshot.mutableWebsite();
        healthWebsite->set_health_check_enabled(true);
        healthWebsite->set_healthy_threshold(1);
        healthWebsite->set_unhealthy_threshold(1);
        healthWebsite->mutable_origins(0)->set_host("127.0.0.1");
        healthWebsite->mutable_origins(0)->set_port(healthPort);
        apply(runtime, std::move(healthSnapshot));
        flexedge::node::OriginHealthRegistry nodeHealth;
        nodeHealth.failure("website-1", "origin-1", 1);
        flexedge::node::OriginHealthSupervisor nodeSupervisor(loops.loop(0), runtime, nodeHealth,
                                                              firstOriginTls);
        nodeSupervisor.requestStart();
        const auto nodeHealthy = waitUntil(
            [&] { return nodeHealth.healthy("website-1", "origin-1"); }, std::chrono::seconds(3));
        if (!nodeHealthy) {
            std::cerr << "health requests=" << healthRequests.load() << '\n';
        }
        REQUIRE(nodeHealthy);
        REQUIRE(healthRequests.load() == 1);
        nodeSupervisor.stop();

        const auto wwwIdentity = makeIdentity("www.example.com", 1);
        const auto apiIdentity = makeIdentity("api.example.com", 2);
        asio::ip::tcp::acceptor tlsOriginAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto tlsOriginPort = tlsOriginAcceptor.local_endpoint().port();
        std::jthread tlsOriginServer([&] {
            for (int requestIndex = 0; requestIndex < 5; ++requestIndex) {
                asio::ip::tcp::socket originSocket(originContext);
                tlsOriginAcceptor.accept(originSocket);
                std::array<char, 4096> originRequest{};
                std::error_code ignored;
                const auto requestSize =
                    originSocket.read_some(asio::buffer(originRequest), ignored);
                const auto requestHead = std::string_view(originRequest.data(), requestSize);
                if (requestHead.contains("GET /large ")) {
                    const std::string body(96 * 1024, 'L');
                    const auto originResponse = std::string("HTTP/1.1 200 OK\r\nContent-Length: ") +
                                                std::to_string(body.size()) +
                                                "\r\nConnection: close\r\n\r\n" + body;
                    asio::write(originSocket, asio::buffer(originResponse));
                } else {
                    constexpr std::string_view originResponse =
                        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nTLS";
                    asio::write(originSocket, asio::buffer(originResponse));
                }
            }
        });
        auto tlsSnapshot = snapshot(false);
        tlsSnapshot.setGeneration(9);
        auto* tlsWebsite = tlsSnapshot.mutableWebsite();
        tlsWebsite->set_hsts_enabled(true);
        tlsWebsite->set_http2_enabled(true);
        tlsWebsite->mutable_origins(0)->set_host("127.0.0.1");
        tlsWebsite->mutable_origins(0)->set_port(tlsOriginPort);
        tlsSnapshot.mutableCertificate(0)->set_certificate_chain_pem(wwwIdentity.certificate);
        tlsSnapshot.mutableCertificate(0)->set_private_key_pem(wwwIdentity.privateKey);
        auto* apiCertificate = tlsSnapshot.addCertificate();
        apiCertificate->set_id("certificate-2");
        apiCertificate->set_certificate_chain_pem(apiIdentity.certificate);
        apiCertificate->set_private_key_pem(apiIdentity.privateKey);
        auto* apiDomain = tlsSnapshot.mutableWebsite()->add_domains();
        apiDomain->set_hostname("api.example.com");
        apiDomain->set_https_enabled(true);
        apiDomain->set_certificate_digest(
            flexedge::node::artifactDigest(tlsSnapshot.objects.back().content()));
        apply(runtime, std::move(tlsSnapshot));
        flexedge::node::TlsContextRegistry tlsContexts;
        tlsContexts.publish(
            std::make_shared<const flexedge::node::TlsContextSet>(*runtime.config()));
        auto httpsListener = std::make_shared<flexedge::node::HttpsListener>(
            loops.loop(0), runtime, nodeHealth, tlsContexts, runtimeMetrics, firstWorkerLogs,
            firstOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto httpsPort = httpsListener->localEndpoint().port();
        httpsListener->requestStart();
        REQUIRE(tlsAlpn(httpsPort, "www.example.com", true) == "h2");
        REQUIRE(tlsAlpn(httpsPort, "www.example.com", false) == "http/1.1");
        const auto http2Response = tlsHttp2Request(httpsPort, "www.example.com");
        REQUIRE(http2Response.peerCommonName == "200");
        REQUIRE(http2Response.bytes == "TLS");
        const auto largeHttp2Response = tlsHttp2Request(httpsPort, "www.example.com", "/large");
        REQUIRE(largeHttp2Response.peerCommonName == "200");
        REQUIRE(largeHttp2Response.bytes == std::string(96 * 1024, 'L'));
        const auto wwwResponse = tlsRequest(httpsPort, "www.example.com");
        REQUIRE(wwwResponse.peerCommonName == "www.example.com");
        REQUIRE(wwwResponse.bytes.ends_with("\r\n\r\nTLS"));
        REQUIRE(wwwResponse.bytes.contains("Strict-Transport-Security: max-age=31536000\r\n"));
        const auto apiResponse = tlsRequest(httpsPort, "api.example.com");
        REQUIRE(apiResponse.peerCommonName == "api.example.com");
        REQUIRE(apiResponse.bytes.starts_with("HTTP/1.1 200 OK\r\n"));

        bool unknownSniRejected = false;
        try {
            (void)tlsRequest(httpsPort, "unknown.example.com");
        } catch (const std::system_error&) {
            unknownSniRejected = true;
        }
        REQUIRE(unknownSniRejected);

        const auto refreshedIdentity = makeIdentity("refreshed.example.com", 3);
        auto refreshedSnapshot = snapshot(false);
        refreshedSnapshot.setGeneration(10);
        auto* refreshedWebsite = refreshedSnapshot.mutableWebsite();
        refreshedWebsite->set_hsts_enabled(true);
        refreshedWebsite->mutable_origins(0)->set_host("127.0.0.1");
        refreshedWebsite->mutable_origins(0)->set_port(tlsOriginPort);
        refreshedSnapshot.mutableCertificate(0)->set_certificate_chain_pem(
            refreshedIdentity.certificate);
        refreshedSnapshot.mutableCertificate(0)->set_private_key_pem(refreshedIdentity.privateKey);
        apply(runtime, std::move(refreshedSnapshot));
        tlsContexts.publish(
            std::make_shared<const flexedge::node::TlsContextSet>(*runtime.config()));
        const auto refreshedResponse = tlsRequest(httpsPort, "www.example.com");
        REQUIRE(refreshedResponse.peerCommonName == "refreshed.example.com");
        REQUIRE(refreshedResponse.bytes.starts_with("HTTP/1.1 200 OK\r\n"));
        httpsListener->requestStop();

        const auto localhostIdentity = makeIdentity("localhost", 4);
        asio::ssl::context tlsOriginServerContext(asio::ssl::context::tls_server);
        tlsOriginServerContext.use_certificate_chain(asio::buffer(localhostIdentity.certificate));
        tlsOriginServerContext.use_private_key(asio::buffer(localhostIdentity.privateKey),
                                               asio::ssl::context::pem);
        asio::ip::tcp::acceptor secureOriginAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto secureOriginPort = secureOriginAcceptor.local_endpoint().port();
        std::jthread secureOriginServer([&] {
            for (int requestIndex = 0; requestIndex < 2; ++requestIndex) {
                asio::ip::tcp::socket socket(originContext);
                secureOriginAcceptor.accept(socket);
                asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                                                                tlsOriginServerContext);
                std::error_code error;
                stream.handshake(asio::ssl::stream_base::server, error);
                if (error) {
                    continue;
                }
                std::array<char, 4096> originRequest{};
                (void)stream.read_some(asio::buffer(originRequest), error);
                if (error) {
                    continue;
                }
                constexpr std::string_view originResponse =
                    "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nSECURE";
                asio::write(stream, asio::buffer(originResponse), error);
            }
        });
        auto secureOriginSnapshot = snapshot(false);
        secureOriginSnapshot.setGeneration(11);
        auto* secureOriginWebsite = secureOriginSnapshot.mutableWebsite();
        secureOriginWebsite->set_http2_enabled(true);
        secureOriginWebsite->set_health_check_enabled(true);
        secureOriginWebsite->set_healthy_threshold(1);
        secureOriginWebsite->set_unhealthy_threshold(1);
        secureOriginWebsite->mutable_origins(0)->set_protocol("https");
        secureOriginWebsite->mutable_origins(0)->set_host("localhost");
        secureOriginWebsite->mutable_origins(0)->set_port(secureOriginPort);
        apply(runtime, std::move(secureOriginSnapshot));
        flexedge::node::OriginTlsContext trustedOriginTls(localhostIdentity.certificate);
        flexedge::node::OriginConnectionPool trustedOriginConnections(loops.loop(0).executor(),
                                                                      trustedOriginTls);
        flexedge::node::OriginHealthRegistry secureOriginHealth;
        auto secureOriginListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(0), runtime, secureOriginHealth, runtimeMetrics, firstWorkerLogs,
            trustedOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto secureEdgePort = secureOriginListener->localEndpoint().port();
        secureOriginListener->requestStart();
        const auto secureOriginResponse = request(secureEdgePort);
        REQUIRE(secureOriginResponse.starts_with("HTTP/1.1 200 OK\r\n"));
        REQUIRE(secureOriginResponse.ends_with("\r\n\r\nSECURE"));

        secureOriginHealth.failure("website-1", "origin-1", 1);
        flexedge::node::OriginHealthSupervisor secureOriginSupervisor(
            loops.loop(0), runtime, secureOriginHealth, trustedOriginTls);
        secureOriginSupervisor.requestStart();
        REQUIRE(waitUntil([&] { return secureOriginHealth.healthy("website-1", "origin-1"); },
                          std::chrono::seconds(3)));
        secureOriginSupervisor.stop();
        secureOriginListener->requestStop();

        asio::ssl::context bufferedOriginServerContext(asio::ssl::context::tls_server);
        bufferedOriginServerContext.use_certificate_chain(
            asio::buffer(localhostIdentity.certificate));
        bufferedOriginServerContext.use_private_key(asio::buffer(localhostIdentity.privateKey),
                                                    asio::ssl::context::pem);
        asio::ip::tcp::acceptor bufferedOriginAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto bufferedOriginPort = bufferedOriginAcceptor.local_endpoint().port();
        std::atomic<int> bufferedOriginConnections{};
        std::atomic<int> bufferedOriginRequests{};
        std::atomic<bool> bufferedOriginUsedHttp1{};
        std::jthread bufferedOriginServer([&] {
            asio::ip::tcp::socket socket(originContext);
            bufferedOriginAcceptor.accept(socket);
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                                                            bufferedOriginServerContext);
            stream.handshake(asio::ssl::stream_base::server);
            ++bufferedOriginConnections;
            bufferedOriginUsedHttp1 =
                flexedge::node::negotiatedHttpProtocol(stream.native_handle()) ==
                flexedge::node::HttpWireProtocol::kHttp1;
            for (int requestIndex = 0; requestIndex < 2; ++requestIndex) {
                std::string input;
                std::array<char, 1024> buffer{};
                while (!input.contains("\r\n\r\n")) {
                    const auto size = stream.read_some(asio::buffer(buffer));
                    input.append(buffer.data(), size);
                }
                REQUIRE(!input.contains("Connection: close\r\n"));
                ++bufferedOriginRequests;
                constexpr std::string_view originResponse =
                    "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nH1ORIGIN";
                asio::write(stream, asio::buffer(originResponse));
            }
        });
        auto bufferedOriginSnapshot = snapshot(false);
        bufferedOriginSnapshot.setGeneration(12);
        auto* bufferedOriginWebsite = bufferedOriginSnapshot.mutableWebsite();
        bufferedOriginWebsite->set_http2_enabled(true);
        bufferedOriginWebsite->set_response_compression_enabled(true);
        bufferedOriginWebsite->set_health_check_enabled(false);
        bufferedOriginWebsite->mutable_origins(0)->set_protocol("https");
        bufferedOriginWebsite->mutable_origins(0)->set_host("localhost");
        bufferedOriginWebsite->mutable_origins(0)->set_port(bufferedOriginPort);
        apply(runtime, std::move(bufferedOriginSnapshot));
        auto bufferedOriginListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(0), runtime, secureOriginHealth, runtimeMetrics, firstWorkerLogs,
            trustedOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto bufferedOriginEdgePort = bufferedOriginListener->localEndpoint().port();
        bufferedOriginListener->requestStart();
        const auto firstBufferedOriginResponse =
            request(bufferedOriginEdgePort, "WWW.Example.COM:80", "Accept-Encoding: gzip\r\n");
        const auto secondBufferedOriginResponse =
            request(bufferedOriginEdgePort, "WWW.Example.COM:80", "Accept-Encoding: gzip\r\n");
        REQUIRE(bufferedOriginUsedHttp1.load());
        REQUIRE(firstBufferedOriginResponse.ends_with("\r\n\r\nH1ORIGIN"));
        REQUIRE(secondBufferedOriginResponse.ends_with("\r\n\r\nH1ORIGIN"));
        REQUIRE(bufferedOriginConnections.load() == 1);
        REQUIRE(bufferedOriginRequests.load() == 2);
        bufferedOriginListener->requestStop();

        asio::ip::tcp::acceptor untrustedOriginAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto untrustedOriginPort = untrustedOriginAcceptor.local_endpoint().port();
        std::jthread untrustedOriginServer([&] {
            asio::ip::tcp::socket socket(originContext);
            untrustedOriginAcceptor.accept(socket);
            asio::ssl::stream<asio::ip::tcp::socket> stream(std::move(socket),
                                                            tlsOriginServerContext);
            std::error_code ignored;
            stream.handshake(asio::ssl::stream_base::server, ignored);
        });
        auto untrustedOriginSnapshot = snapshot(false);
        untrustedOriginSnapshot.setGeneration(13);
        auto* untrustedOrigin = untrustedOriginSnapshot.mutableWebsite()->mutable_origins(0);
        untrustedOrigin->set_protocol("https");
        untrustedOrigin->set_host("localhost");
        untrustedOrigin->set_port(untrustedOriginPort);
        apply(runtime, std::move(untrustedOriginSnapshot));
        const auto untrustedOriginResponse = request(port);
        REQUIRE(untrustedOriginResponse.starts_with("HTTP/1.1 502 Bad Gateway\r\n"));

        asio::ip::tcp::acceptor webSocketAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto webSocketPort = webSocketAcceptor.local_endpoint().port();
        std::atomic<bool> upgradeHeadersPreserved{};
        std::jthread webSocketOrigin([&] {
            asio::ip::tcp::socket socket(originContext);
            webSocketAcceptor.accept(socket);
            std::string handshake;
            std::array<char, 1024> buffer{};
            while (!handshake.contains("\r\n\r\n")) {
                const auto size = socket.read_some(asio::buffer(buffer));
                handshake.append(buffer.data(), size);
            }
            upgradeHeadersPreserved =
                handshake.contains("Upgrade: websocket\r\n") &&
                handshake.contains("Connection: keep-alive, Upgrade\r\n") &&
                handshake.contains("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n");
            constexpr std::string_view switchingProtocols =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n";
            asio::write(socket, asio::buffer(switchingProtocols));
            std::array<char, 4> clientBytes{};
            asio::read(socket, asio::buffer(clientBytes));
            if (std::string_view(clientBytes.data(), clientBytes.size()) == "PING") {
                asio::write(socket, asio::buffer(std::string_view("PONG")));
            }
        });
        auto webSocketSnapshot = snapshot(false);
        webSocketSnapshot.setGeneration(14);
        auto* webSocketOriginConfig = webSocketSnapshot.mutableWebsite()->mutable_origins(0);
        webSocketOriginConfig->set_host("127.0.0.1");
        webSocketOriginConfig->set_port(webSocketPort);
        apply(runtime, std::move(webSocketSnapshot));
        REQUIRE(webSocketRoundTrip(port) == "PONG");
        REQUIRE(upgradeHeadersPreserved.load());

        asio::ip::tcp::acceptor reusableOriginAcceptor(
            originContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto reusableOriginPort = reusableOriginAcceptor.local_endpoint().port();
        std::atomic<int> reusableOriginConnections{};
        std::atomic<int> reusableOriginRequests{};
        std::jthread reusableOriginServer([&] {
            asio::ip::tcp::socket socket(originContext);
            reusableOriginAcceptor.accept(socket);
            ++reusableOriginConnections;
            for (int requestIndex = 0; requestIndex < 2; ++requestIndex) {
                std::string input;
                std::array<char, 1024> buffer{};
                while (!input.contains("\r\n\r\n")) {
                    const auto size = socket.read_some(asio::buffer(buffer));
                    input.append(buffer.data(), size);
                }
                REQUIRE(!input.contains("Connection: close\r\n"));
                ++reusableOriginRequests;
                constexpr std::string_view originResponse =
                    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nREUSE";
                asio::write(socket, asio::buffer(originResponse));
            }
        });
        auto reusableOriginSnapshot = snapshot(false);
        reusableOriginSnapshot.setGeneration(15);
        auto* reusableWebsite = reusableOriginSnapshot.mutableWebsite();
        reusableWebsite->set_http2_enabled(false);
        reusableWebsite->set_response_compression_enabled(false);
        reusableWebsite->set_health_check_enabled(false);
        reusableWebsite->mutable_origins(0)->set_host("127.0.0.1");
        reusableWebsite->mutable_origins(0)->set_port(reusableOriginPort);
        apply(runtime, std::move(reusableOriginSnapshot));
        flexedge::node::OriginHealthRegistry reusableOriginHealth;
        auto reusableEdgeListener = std::make_shared<flexedge::node::HttpListener>(
            loops.loop(0), runtime, reusableOriginHealth, runtimeMetrics, firstWorkerLogs,
            firstOriginConnections, requestBuffers, responseBuffers,
            asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        const auto reusableEdgePort = reusableEdgeListener->localEndpoint().port();
        reusableEdgeListener->requestStart();
        const auto firstReusedResponse = request(reusableEdgePort);
        const auto secondReusedResponse = request(reusableEdgePort);
        REQUIRE(firstReusedResponse.ends_with("\r\n\r\nREUSE"));
        REQUIRE(secondReusedResponse.ends_with("\r\n\r\nREUSE"));
        REQUIRE(reusableOriginConnections.load() == 1);
        REQUIRE(reusableOriginRequests.load() == 2);
        reusableEdgeListener->requestStop();
        firstListener->stop();
        secondListener->stop();
        loops.stop();
        loops.join();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#undef REQUIRE
