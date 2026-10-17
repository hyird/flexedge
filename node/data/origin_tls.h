#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/ssl/stream.hpp>
#include <openssl/ssl.h>

namespace flexedge::node {

class origin_tls_context final {
public:
    using stream = asio::ssl::stream<asio::ip::tcp::socket>;

    explicit origin_tls_context(std::string_view certificate_authority = {})
        : context_(asio::ssl::context::tls_client) {
        context_.set_options(asio::ssl::context::default_workarounds |
                             asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                             asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
        context_.set_verify_mode(asio::ssl::verify_peer);
        if (certificate_authority.empty()) {
            context_.set_default_verify_paths();
        } else {
            context_.add_certificate_authority(asio::buffer(certificate_authority));
        }
    }

    [[nodiscard]] stream get_stream(asio::ip::tcp::socket socket, std::string_view hostname, bool http2 = false) {
        stream result(std::move(socket), context_);
        const std::string server_name(hostname);
        if (SSL_set_tlsext_host_name(result.native_handle(), server_name.c_str()) != 1) {
            throw std::runtime_error("could not configure origin TLS SNI");
        }
        static constexpr std::array<unsigned char, 9> http1{8, 'h', 't', 't', 'p',
            '/', '1', '.', '1'};
        static constexpr std::array<unsigned char, 12> protocols{2,'h','2',8,'h','t','t','p','/','1','.','1'};
        if (SSL_set_alpn_protos(result.native_handle(), http2 ? protocols.data() : http1.data(),
                static_cast<unsigned int>(http2 ? protocols.size() : http1.size())) != 0) {
            throw std::runtime_error("could not configure origin TLS ALPN");
        }
        result.set_verify_callback(asio::ssl::host_name_verification(server_name));
        return result;
    }

private:
    asio::ssl::context context_;
};

}  // namespace flexedge::node
