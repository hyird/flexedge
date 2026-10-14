#pragma once

#include <atomic>
#include <cctype>
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

#include "node/runtime/compiled_config.h"

namespace flexedge::node {

class tls_context_set final {
public:
    explicit tls_context_set(const compiled_config& config) {
        if (!config.enabled()) {
            return;
        }
        for (const auto* website : config.websites()) {
            if (!website->enabled() || !website->https_enabled()) {
                continue;
            }
            for (const auto& domain : website->domains()) {
                if (!domain.https_enabled()) {
                    continue;
                }
                const auto* certificate = config.certificate(domain.certificate_digest());
                if (certificate == nullptr) {
                    throw std::runtime_error("HTTPS domain certificate is missing");
                }
                auto context = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);
                configure(*context, *certificate, website->minimum_tls_version());
                auto* native = context->native_handle();
                const auto hostname = normalize_hostname(domain.hostname());
                if (!sni_.emplace(hostname, native).second) {
                    throw std::runtime_error("duplicate TLS SNI hostname");
                }
                contexts_.push_back(std::move(context));
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

    [[nodiscard]] asio::ssl::context& default_context() const {
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
        static constexpr unsigned char http1[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        return SSL_select_next_proto(const_cast<unsigned char**>(output), output_size, http1,
                   static_cast<unsigned int>(sizeof(http1)), input,
                   input_size) == OPENSSL_NPN_NEGOTIATED
                   ? SSL_TLSEXT_ERR_OK
                   : SSL_TLSEXT_ERR_NOACK;
    }

    static int select_context(SSL* ssl, int*, void* argument) noexcept {
        const auto* set = static_cast<const tls_context_set*>(argument);
        const auto* raw = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (raw == nullptr) {
            return set->sni_.size() == 1 ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        std::string hostname(raw);
        for (auto& ch : hostname) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        const auto found = set->sni_.find(hostname);
        if (found == set->sni_.end()) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (SSL_set_SSL_CTX(ssl, found->second) == nullptr ||
            SSL_set_min_proto_version(ssl, SSL_CTX_get_min_proto_version(found->second)) != 1) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        return SSL_TLSEXT_ERR_OK;
    }

    std::vector<std::unique_ptr<asio::ssl::context>> contexts_;
    std::unordered_map<std::string, SSL_CTX*> sni_;
};

class tls_context_registry final {
public:
    void publish(std::shared_ptr<const tls_context_set> contexts) noexcept {
        contexts_.store(std::move(contexts), std::memory_order_release);
    }

    [[nodiscard]] std::shared_ptr<const tls_context_set> current() const noexcept {
        return contexts_.load(std::memory_order_acquire);
    }

private:
    std::atomic<std::shared_ptr<const tls_context_set>> contexts_;
};

}  // namespace flexedge::node
