#include "service/config/http_server.h"

#include <ruvia/web/app.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/streaming.h>
#include <ruvia/web/websocket.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

namespace {

void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("could not generate test TLS identity");
    }
}

void generate_identity(const std::filesystem::path& directory) {
    const auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", 2048), &EVP_PKEY_free);
    const auto certificate = std::unique_ptr<X509, decltype(&X509_free)>(X509_new(), &X509_free);
    require(key != nullptr && certificate != nullptr);
    require(X509_set_version(certificate.get(), 2) == 1);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    auto* subject = X509_get_subject_name(certificate.get());
    require(subject != nullptr);
    require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1);
    require(X509_set_issuer_name(certificate.get(), subject) == 1);
    const auto alternative_names = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"), &X509_EXTENSION_free);
    require(alternative_names != nullptr && X509_add_ext(certificate.get(), alternative_names.get(), -1) == 1);
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    const auto certificate_output = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new_file((directory / "certificate.pem").string().c_str(), "w"), &BIO_free);
    const auto key_output = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new_file((directory / "key.pem").string().c_str(), "w"), &BIO_free);
    require(certificate_output != nullptr && key_output != nullptr);
    require(PEM_write_bio_X509(certificate_output.get(), certificate.get()) == 1);
    require(PEM_write_bio_PrivateKey(key_output.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
}

class http_server_controller final {
public:
    RUVIA_ROUTES_BEGIN(http_server_controller)
    RUVIA_GET("/api/health", health);
    RUVIA_POST("/echo", echo);
    RUVIA_GET_SSE("/events", events);
    RUVIA_GET_WS("/socket", socket);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> health(ruvia::context& context) {
        co_return context.text("ready");
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& context) {
        const auto body = co_await context.req().text();
        co_return context.text(body);
    }

    ruvia::task<void> events(ruvia::context& context) {
        auto stream = context.stream_sse();
        co_await stream.write({.data_ = "ready", .event_ = "snapshot"});
    }

    ruvia::task<void> socket(ruvia::context& context) {
        auto& connection = context.get_websocket();
        co_await connection.text("ready");
    }
};

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) {
            throw std::runtime_error("expected --generate-identity DIRECTORY, --inspect ENV_FILE, --check ENV_FILE or --serve ENV_FILE");
        }
        const std::string_view mode(argv[1]);
        if (mode == "--generate-identity") {
            generate_identity(argv[2]);
            return 0;
        }
        auto& app = ruvia::app();
        const auto env_path = std::filesystem::absolute(argv[2]);
        app.load_dotenv(env_path, {.existing_variables_ = ruvia::dotenv_existing_variable_policy::override, .missing_file_ = ruvia::dotenv_missing_file_policy::require});
        const auto settings = service::config::get_http_server_settings(app.env(), env_path.parent_path());
        std::cout << "http=" << *settings.listen_.http_ << " https=" << settings.listen_.https_.value_or(0) << " redirect=" << settings.listen_.auto_https_redirect_ << '\n';
        if (mode == "--inspect") {
            return 0;
        }
        app.listen(settings.listen_);
        if (mode == "--check") {
            return 0;
        }
        if (mode != "--serve") {
            throw std::runtime_error("unsupported fixture mode");
        }
        app.server({.worker_count_ = 2, .process_signal_handlers_ = ruvia::process_signal_handler_policy::install});
        app.document_root({.root_ = env_path.parent_path()});
        app.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
