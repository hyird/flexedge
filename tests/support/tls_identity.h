#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/stream.hpp>
#include <asio/write.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace flexedge::testing {

inline void require_tls_identity(bool condition) {
    if (!condition) {
        throw std::runtime_error("could not create or use the test TLS identity");
    }
}

struct pem_identity final {
    std::string certificate_{};
    std::string private_key_{};
};

inline pem_identity make_identity(std::string_view common_name, std::int64_t serial) {
    const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(
        EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048), &EVP_PKEY_free);
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), &X509_free);
    require_tls_identity(key != nullptr);
    require_tls_identity(certificate != nullptr);
    require_tls_identity(X509_set_version(certificate.get(), 2) == 1);
    require_tls_identity(ASN1_INTEGER_set_int64(X509_get_serialNumber(certificate.get()), serial) == 1);
    require_tls_identity(X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) != nullptr);
    require_tls_identity(X509_gmtime_adj(X509_get_notAfter(certificate.get()), 3600) != nullptr);
    require_tls_identity(X509_set_pubkey(certificate.get(), key.get()) == 1);
    auto* subject = X509_get_subject_name(certificate.get());
    require_tls_identity(subject != nullptr);
    require_tls_identity(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char*>(common_name.data()),
                             static_cast<int>(common_name.size()), -1, 0) == 1);
    require_tls_identity(X509_set_issuer_name(certificate.get(), subject) == 1);
    require_tls_identity(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);

    const auto certificate_bio =
        std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    const auto key_bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    require_tls_identity(certificate_bio != nullptr);
    require_tls_identity(key_bio != nullptr);
    require_tls_identity(PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1);
    require_tls_identity(PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                             nullptr) == 1);
    char* certificate_bytes{};
    char* key_bytes{};
    const auto certificate_size = BIO_get_mem_data(certificate_bio.get(), &certificate_bytes);
    const auto key_size = BIO_get_mem_data(key_bio.get(), &key_bytes);
    require_tls_identity(certificate_size > 0);
    require_tls_identity(key_size > 0);
    return {
        .certificate_ = std::string(certificate_bytes, static_cast<std::size_t>(certificate_size)),
        .private_key_ = std::string(key_bytes, static_cast<std::size_t>(key_size)),
    };
}

struct tls_response final {
    std::string bytes_{};
    std::string peer_common_name_{};
};

inline tls_response tls_request(std::uint16_t port, std::string_view server_name, std::string_view host = {}) {
    asio::io_context context;
    asio::ssl::context tls_context(asio::ssl::context::tls_client);
    tls_context.set_verify_mode(asio::ssl::verify_none);
    asio::ssl::stream<asio::ip::tcp::socket> stream(context, tls_context);
    require_tls_identity(SSL_set_tlsext_host_name(stream.native_handle(), std::string(server_name).c_str()) == 1);
    stream.lowest_layer().connect({asio::ip::address_v4::loopback(), port});
    stream.handshake(asio::ssl::stream_base::client);
    const auto peer = std::unique_ptr<X509, decltype(&X509_free)>(
        SSL_get1_peer_certificate(stream.native_handle()), &X509_free);
    require_tls_identity(peer != nullptr);
    std::array<char, 256> common_name{};
    const auto common_name_size =
        X509_NAME_get_text_by_NID(X509_get_subject_name(peer.get()), NID_commonName,
            common_name.data(), static_cast<int>(common_name.size()));
    require_tls_identity(common_name_size > 0);
    const auto bytes =
        "GET /tls HTTP/1.1\r\nHost: " + std::string(host.empty() ? server_name : host) + "\r\nConnection: close\r\n\r\n";
    asio::write(stream, asio::buffer(bytes));
    tls_response response{
        .peer_common_name_ = std::string(common_name.data(), static_cast<std::size_t>(common_name_size))};
    std::array<char, 1024> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = stream.read_some(asio::buffer(buffer), error);
        response.bytes_.append(buffer.data(), size);
        if (error == asio::error::eof || error == asio::ssl::error::stream_truncated) {
            break;
        }
        if (error) {
            throw std::system_error(error, "could not read HTTPS edge response");
        }
    }
    return response;
}

}  // namespace flexedge::testing
