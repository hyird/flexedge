#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/context_base.hpp>
#include <openssl/ssl.h>

#include "common/route_policy.h"
#include "node/data/http_protocol.h"
#include "node/runtime/compiled_config.h"

namespace flexedge::node {

class tls_context_set final {
public:
    explicit tls_context_set(const compiled_config& config) {
        if (!config.enabled()) {
            return;
        }
        for (const auto* website : config.websites()) {
            if (!website->enabled()) {
                continue;
            }
            for (const auto& domain : website->domains()) {
                SSL_CTX* native{};
                if (domain.https_enabled()) {
                    const auto* certificate = config.certificate(domain.certificate_digest());
                    if (certificate == nullptr) {
                        throw std::runtime_error("HTTPS domain certificate is missing");
                    }
                    auto context = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
                    configure(*context, *certificate, website->minimum_tls_version());
                    native = context->native_handle();
                    contexts_.push_back(std::move(context));
                }
                // Keep HTTP-only bindings so an exact match cannot fall through
                // to a broader HTTPS wildcard.
                const auto hostname = flexedge::route_policy::hostname(domain.hostname());
                if (!sni_.emplace(hostname, native).second) {
                    throw std::runtime_error("duplicate TLS SNI hostname");
                }
            }
        }
        if (!contexts_.empty()) {
            SSL_CTX_set_tlsext_servername_callback(contexts_.front()->native_handle(),
                &select_context);
            SSL_CTX_set_tlsext_servername_arg(contexts_.front()->native_handle(), this);
        }
    }

    // OpenSSL's SNI callback retains this object's address.
    tls_context_set(const tls_context_set&) = delete;
    tls_context_set& operator=(const tls_context_set&) = delete;
    tls_context_set(tls_context_set&&) = delete;
    tls_context_set& operator=(tls_context_set&&) = delete;

    [[nodiscard]] bool empty() const noexcept {
        return contexts_.empty();
    }

    [[nodiscard]] asio::ssl::context& default_context() {
        if (contexts_.empty()) {
            throw std::logic_error("TLS context set is empty");
        }
        return *contexts_.front();
    }

private:
    static void configure(asio::ssl::context& context, const v2::Certificate& certificate,
        std::string_view minimum_version) {
        context.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                            asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                            asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
        SSL_CTX_set_options(context.native_handle(), SSL_OP_NO_COMPRESSION);
        const auto version = minimum_version == "1.3" ? TLS1_3_VERSION : TLS1_2_VERSION;
        if (SSL_CTX_set_min_proto_version(context.native_handle(), version) != 1) {
            throw std::runtime_error("could not configure minimum TLS version");
        }
        context.use_certificate_chain(asio::buffer(certificate.certificate_chain_pem()));
        context.use_private_key(asio::buffer(certificate.private_key_pem()),
            asio::ssl::context::pem);
        if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
            throw std::runtime_error("TLS certificate and private key do not match");
        }
        SSL_CTX_set_alpn_select_cb(context.native_handle(), &select_alpn, nullptr);
    }

    static int select_alpn(SSL*, const unsigned char** output, unsigned char* output_size,
        const unsigned char* input, unsigned int input_size, void*) noexcept {
        static constexpr unsigned char protocols[] = {2,'h','2',8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        return SSL_select_next_proto(const_cast<unsigned char**>(output), output_size, protocols,
                   static_cast<unsigned int>(sizeof(protocols)), input,
                   input_size) == OPENSSL_NPN_NEGOTIATED
                   ? SSL_TLSEXT_ERR_OK
                   : SSL_TLSEXT_ERR_NOACK;
    }

    static int select_context(SSL* ssl, int* alert, void* argument) noexcept {
        const auto* set = static_cast<const tls_context_set*>(argument);
        const auto* hello = current_tls_client_hello(ssl);
        if (hello == nullptr || hello->server_name_.empty()) {
            *alert = SSL_version(ssl) >= TLS1_3_VERSION ? SSL_AD_MISSING_EXTENSION : SSL_AD_HANDSHAKE_FAILURE;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        const auto& raw = hello->server_name_;
        *alert = SSL_AD_UNRECOGNIZED_NAME;
        if (!flexedge::route_policy::exact_hostname(raw)) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        try {
            const auto found = flexedge::route_policy::find_hostname(set->sni_,
                flexedge::route_policy::hostname(raw));
            if (found == set->sni_.end() || found->second == nullptr) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            if (SSL_set_SSL_CTX(ssl, found->second) == nullptr ||
                SSL_set_min_proto_version(ssl, SSL_CTX_get_min_proto_version(found->second)) != 1) {
                *alert = SSL_AD_INTERNAL_ERROR;
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
        } catch (...) {
            *alert = SSL_AD_INTERNAL_ERROR;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        return SSL_TLSEXT_ERR_OK;
    }

    std::vector<std::unique_ptr<asio::ssl::context>> contexts_;
    std::unordered_map<std::string, SSL_CTX*> sni_;
};

class tls_context_registry final {
public:
    void publish(std::shared_ptr<tls_context_set> contexts) noexcept {
        contexts_ = std::move(contexts);
    }

    [[nodiscard]] std::shared_ptr<tls_context_set> current() noexcept {
        return contexts_;
    }

private:
    // Worker-local OpenSSL runtime. Only compiled certificate material crosses loops.
    std::shared_ptr<tls_context_set> contexts_;
};

}  // namespace flexedge::node
