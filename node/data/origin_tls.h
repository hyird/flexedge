#pragma once

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/ip/tcp.hpp>
#include <asio/buffer.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/host_name_verification.hpp>
#include <asio/ssl/stream.hpp>

#include <openssl/ssl.h>

#include "node/data/http_protocol.h"

namespace flexedge::node {

class OriginTlsContext final {
  public:
    using Stream = asio::ssl::stream<asio::ip::tcp::socket>;

    explicit OriginTlsContext(std::string_view certificateAuthority = {})
        : context_(asio::ssl::context::tls_client) {
        context_.set_options(asio::ssl::context::default_workarounds |
                             asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                             asio::ssl::context::no_tlsv1 | asio::ssl::context::no_tlsv1_1);
        context_.set_verify_mode(asio::ssl::verify_peer);
        if (certificateAuthority.empty()) {
            context_.set_default_verify_paths();
        } else {
            context_.add_certificate_authority(asio::buffer(certificateAuthority));
        }
    }

    [[nodiscard]] Stream stream(asio::ip::tcp::socket socket, std::string_view hostname,
                                bool http2Enabled) {
        Stream result(std::move(socket), context_);
        const std::string serverName(hostname);
        if (SSL_set_tlsext_host_name(result.native_handle(), serverName.c_str()) != 1) {
            throw std::runtime_error("could not configure origin TLS SNI");
        }
        static constexpr std::array<unsigned char, 9> http1{8,   'h', 't', 't', 'p',
                                                            '/', '1', '.', '1'};
        static constexpr std::array<unsigned char, 12> adaptive{2,   'h', '2', 8,   'h', 't',
                                                                't', 'p', '/', '1', '.', '1'};
        const auto* protocols = http2Enabled ? adaptive.data() : http1.data();
        const auto size = http2Enabled ? adaptive.size() : http1.size();
        if (SSL_set_alpn_protos(result.native_handle(), protocols,
                                static_cast<unsigned int>(size)) != 0) {
            throw std::runtime_error("could not configure origin TLS ALPN");
        }
        result.set_verify_callback(asio::ssl::host_name_verification(serverName));
        return result;
    }

  private:
    asio::ssl::context context_;
};

} // namespace flexedge::node
