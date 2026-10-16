#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincrypt.h>
#include <windows.h>
#include <winsock2.h>
#endif

#include <ruvia/core/task.h>
#include <ruvia/http/http_known_method.h>
#include <ruvia/http/http_status.h>
#include <ruvia/web/app.h>
#include <ruvia/web/context.h>
#include <ruvia/web/controller.h>
#include <ruvia/web/db/db.h>
#include <ruvia/web/db/db_migration.h>
#include <ruvia/web/error.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "service/common/http.h"
#include "service/config/auth.h"
#include "service/config/database.h"
#include "service/config/geoip.h"
#include "service/config/http_server.h"
#include "service/config/outbound.h"
#include "service/config/rate_limit.h"
#include "service/config/redis.h"
#include "service/config/runtime_env.h"
#include "service/config/schema.h"
#include "service/features/geoip/runtime_database.h"
#include "service/features/live_resource/notification_outbox.h"
#include "service/features/logging/access.h"
#include "service/features/logging/connection.h"
#include "service/features/logging/logger.h"
#include "service/features/node_presence/notification_relay.h"
#include "service/middleware/rate_limit.h"

// 业务控制器（RUVIA_CONTROLLER_GROUP 在静态阶段把路由表注册到 ruvia::app()）。
#include "service/domains/agent/agent_controller.h"
#include "service/domains/auth/auth_controller.h"
#include "service/domains/certificate/certificate_controller.h"
#include "service/domains/cluster/cluster_controller.h"
#include "service/domains/dns_zone/dns_zone_controller.h"
#include "service/domains/node/node_controller.h"
#include "service/domains/overview/overview_controller.h"
#include "service/domains/provider/provider_controller.h"
#include "service/domains/task/task_controller.h"
#include "service/domains/website/website_controller.h"
#include "service/features/background/task_context.h"
#include "service/features/background/work_available.h"
#include "service/features/certificate/worker.h"
#include "service/features/dns_sync/worker.h"
#include "service/features/initial_admin/bootstrap.h"
#include "service/features/live_resource/notification_reader.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/notification_relay.h"
#include "service/features/log_ingest/worker.h"
#include "service/features/node_channel/projection.h"
#include "service/features/node_channel/report_persistence.h"
#include "service/features/node_channel/report_waits.h"
#include "service/features/node_dispatch/fanout.h"
#include "service/features/node_release/catalog.h"
#include "service/features/provider_verification/worker.h"
#include "service/features/website_dns/worker.h"
#include "service/utils/secret.h"
#include "service/utils/token.h"

namespace {

std::filesystem::path executable_dir(const char* executable_path) {
    if (executable_path == nullptr || executable_path[0] == '\0') {
        return std::filesystem::current_path();
    }

    std::filesystem::path path(executable_path);
    if (path.is_relative()) {
        path = std::filesystem::current_path() / path;
    }

    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (!ec) {
        path = std::move(canonical);
    }

    const auto dir = path.parent_path();
    return dir.empty() ? std::filesystem::current_path() : dir;
}

std::filesystem::path& web_root_path() {
    static std::filesystem::path path;
    return path;
}

// file_response_options borrows its path, so the SPA entry lives in named static storage.
std::filesystem::path& web_index_path() {
    static std::filesystem::path path;
    return path;
}

void configure_document_root(ruvia::application& app, const std::filesystem::path& runtime_dir) {
    auto web_root = runtime_dir / "web";
    if (!std::filesystem::is_directory(web_root)) {
        return;
    }

    ruvia::document_root_config config{
        .root_ = std::move(web_root),
        .static_options_ =
            {
                .cache_control_ = "no-cache",
                .index_file_ = "index.html",
            },
        .precompress_gzip_ = true,
        .precompress_max_bytes_ = 64u * 1024u * 1024u,
    };
    web_root_path() = config.root_;
    web_index_path() = config.root_ / "index.html";
    app.document_root(std::move(config));
}

void log_migration_report(const ruvia::db_migration_report& report) {
    service::logging::info("DB migrations applied=" + std::to_string(report.applied().size()) +
                           ", skipped=" + std::to_string(report.skipped().size()));
}

ruvia::db_config configure_database(ruvia::application& app) {
    auto db_config = service::config::database_config(app.env());
    const ruvia::db_migrator_options migration_options{.table_ = "schema_migrations"};
    log_migration_report(
        ruvia::db_migrator::migrate(db_config, service::config::schema_migrations, migration_options));
    app.database({.alias_ = "default", .config_ = db_config});
    return db_config;
}

ruvia::redis_config configure_redis(ruvia::application& app) {
    auto config = service::config::redis_config(app.env());
    app.redis({.config_ = config});
    return config;
}

// Every Web Worker owns its live delivery state; nothing is shared between
// workers. Changes arrive only through the worker's own Redis readers.
void configure_worker_states(ruvia::application& app, const service::node_release::snapshot& node_release) {
    app.use_worker_state<service::live_resource::hub>();
    app.use_worker_state<service::node_dispatch::fanout::hub>();
    app.use_worker_state<service::log_ingest::fanout::hub>();
    app.use_worker_state<service::node_channel::report_waits>();
    app.use_worker_state<service::background::work_available::claim_signals>();
    app.use_worker_state<service::node_release::catalog>(
        [node_release] { return service::node_release::catalog(node_release); });
}

std::filesystem::path system_ca_bundle([[maybe_unused]] const std::filesystem::path& runtime_dir) {
#ifdef _WIN32
    const auto path = runtime_dir / "system-ca-bundle.pem";
    FILE* output = nullptr;
    if (_wfopen_s(&output, path.c_str(), L"wb") != 0 || output == nullptr) {
        throw std::runtime_error("failed to create Windows system CA bundle");
    }

    auto* bio = BIO_new_fp(output, BIO_NOCLOSE);
    if (bio == nullptr) {
        std::fclose(output);
        throw std::runtime_error("failed to initialize Windows system CA bundle");
    }

    std::size_t certificate_count = 0;
    const auto append_store = [&](DWORD location) {
        const auto store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
            location | CERT_STORE_OPEN_EXISTING_FLAG, L"ROOT");
        if (store == nullptr) {
            return;
        }
        PCCERT_CONTEXT context = nullptr;
        while ((context = CertEnumCertificatesInStore(store, context)) != nullptr) {
            const unsigned char* encoded = context->pbCertEncoded;
            auto* certificate = d2i_X509(nullptr, &encoded, context->cbCertEncoded);
            if (certificate == nullptr) {
                continue;
            }
            if (PEM_write_bio_X509(bio, certificate) == 1) {
                ++certificate_count;
            }
            X509_free(certificate);
        }
        CertCloseStore(store, 0);
    };
    append_store(CERT_SYSTEM_STORE_LOCAL_MACHINE);
    append_store(CERT_SYSTEM_STORE_CURRENT_USER);
    BIO_free(bio);
    std::fclose(output);
    if (certificate_count == 0) {
        throw std::runtime_error("Windows system CA store is empty");
    }
    return path;
#else
    return {};
#endif
}

void configure_outbound_origins(ruvia::application& app, const std::filesystem::path& runtime_dir) {
    const auto ca_file = system_ca_bundle(runtime_dir).string();
    auto origins = service::config::make_outbound_origins(ca_file);
    app.get_http_client({.alias_ = std::string(service::config::cloudflare_origin_alias),
                            .config_ = origins.cloudflare_})
        .get_http_client({.alias_ = std::string(service::config::aliyun_dns_origin_alias),
            .config_ = origins.aliyun_dns_})
        .get_http_client({.alias_ = std::string(service::config::lets_encrypt_origin_alias),
            .config_ = origins.lets_encrypt_})
        .get_http_client({.alias_ = std::string(service::config::zero_ssl_acme_origin_alias),
            .config_ = origins.zero_ssl_acme_})
        .get_http_client({.alias_ = std::string(service::config::zero_ssl_api_origin_alias),
            .config_ = origins.zero_ssl_api_})
        .get_http_client({.alias_ = std::string(service::config::ali_dns_doh_origin_alias),
            .config_ = origins.ali_dns_doh_});
    service::config::configure_outbound_origins(std::move(origins));
}

bool is_spa_request(ruvia::context& c) {
    if (web_root_path().empty()) {
        return false;
    }
    if (c.req().known_method() != ruvia::http_known_method::get &&
        c.req().known_method() != ruvia::http_known_method::head) {
        return false;
    }

    auto path = c.req().path();
    if (path.starts_with("/api/") || path == "/api" || path.starts_with("/assets/")) {
        return false;
    }

    const auto last_slash = path.rfind('/');
    const auto last_segment =
        last_slash == std::string_view::npos ? path : path.substr(last_slash + 1);
    return last_segment.find('.') == std::string_view::npos;
}

bool is_spa_fallback_request(ruvia::context& c, ruvia::http_error_info info) {
    return info.status() == ruvia::http_status::not_found && is_spa_request(c);
}

ruvia::task<ruvia::http_response> handle_not_found(ruvia::context& c) {
    if (is_spa_request(c)) {
        c.header("cache-control", "no-cache");
        co_return c.file(
            {.path_ = web_index_path(), .content_type_ = "text/html; charset=UTF-8"});
    }
    c.status(ruvia::http_status::not_found);
    co_return c.json(service::common::error(
        c, service::common::normalize_business_error_code({}, ruvia::http_status::not_found.value()),
        ruvia::http_reason_phrase(ruvia::http_status::not_found)));
}

// 与 ruvia::http_error_handler_type 签名匹配；运行时把业务 http_error 转成统一响应 DTO。
ruvia::task<ruvia::http_response> handle_error(ruvia::context& c, ruvia::http_error_info info) {
    if (is_spa_fallback_request(c, info)) {
        c.header("cache-control", "no-cache");
        co_return c.file(
            {.path_ = web_index_path(), .content_type_ = "text/html; charset=UTF-8"});
    }

    const auto status = info.status();
    if (status.is_server_error()) {
        std::string diagnostic;
        if (const auto exception = c.exception()) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& error) {
                diagnostic = error.what();
            } catch (...) {
                diagnostic = "non-standard exception";
            }
        }
        if (diagnostic.empty()) {
            diagnostic = info.message().empty() ? std::string(ruvia::http_reason_phrase(status))
                                                : std::string(info.message());
        }
        service::logging::error("Unhandled error: " + diagnostic);
    }
    c.status(status);
    co_return c.json(service::common::error(
        c, service::common::normalize_business_error_code(info.code(), status.value()),
        service::common::response_error_message(info)));
}

void configure_http_server(ruvia::application& app, const std::filesystem::path& configuration_dir) {
    const auto settings = service::config::get_http_server_settings(app.env(), configuration_dir);
    app.get_blocking_pool({
                              .thread_count_ = settings.blocking_threads_,
                              .queue_capacity_ = settings.blocking_queue_capacity_,
                          })
        .compression({.min_bytes_ = settings.compression_min_bytes_})
        .listen(settings.listen_)
        // Server limits Ruvia 8a14af03 did not have are pinned to that release's
        // behavior: stop() closes connections at once (no graceful drain), request
        // heads/bodies have no absolute completion deadline (the 60s inactivity
        // timeouts still apply), and inbound buffering has no aggregate cap beyond
        // the unchanged 16 MiB body / WebSocket message limits. SSE streams and
        // the node control WebSocket therefore keep their previous lifetime.
        .server({
            .worker_count_ = settings.worker_count_,
            .process_signal_handlers_ = ruvia::process_signal_handler_policy::install,
            .drain_timeout_ = std::chrono::milliseconds::zero(),
            .header_completion_timeout_ = std::nullopt,
            .body_completion_timeout_ = std::nullopt,
            .max_inbound_buffer_bytes_per_worker_ = std::numeric_limits<std::size_t>::max(),
            .max_inbound_buffer_bytes_per_connection_ = std::numeric_limits<std::size_t>::max(),
        })
        .on_not_found(&handle_not_found)
        .on_error(&handle_error)
        .on_access(ruvia::access_log_callback_type(service::logging::access_logger{}))
        .on_connection_failure(
            ruvia::connection_failure_callback_type(service::logging::connection_failure_logger{}));
    service::logging::info("Server HTTP starting on " + settings.listen_.address_ + ":" +
                           std::to_string(*settings.listen_.http_) +
                           (settings.listen_.auto_https_redirect_ ? " (redirect to HTTPS)" : ""));
    if (settings.listen_.https_.has_value()) {
        service::logging::info("Server HTTPS starting on " + settings.listen_.address_ + ":" +
                               std::to_string(*settings.listen_.https_));
    }
}

}  // namespace

// 健康检查。
class health_controller final {
public:
    RUVIA_ROUTES_BEGIN(health_controller)
    RUVIA_GET("/api/health", health);
    RUVIA_ROUTES_END

private:
    ruvia::task<ruvia::http_response> health(ruvia::context& c) {
        co_return c.json(service::common::health(c));
    }
};

int main(int argc, char* argv[]) {
    try {
        auto& app = ruvia::app();
        service::logging::session logging_session;
        const auto runtime_dir = executable_dir(argc > 0 ? argv[0] : nullptr);
#ifdef _WIN32
        const auto node_binary_path = runtime_dir / "node.exe";
#else
        const auto node_binary_path = runtime_dir / "node";
#endif
        const auto node_release = service::node_release::load(node_binary_path,
            runtime_dir / "install-node.sh", runtime_dir / "node-release.manifest");
        const auto env_path = service::config::dotenv_path(runtime_dir);
        app.load_dotenv(env_path, {.missing_file_ = ruvia::dotenv_missing_file_policy::require});
        if (service::config::ensure_runtime_secrets(env_path, app.env())) {
            app.load_dotenv(env_path);
        }
        service::utils::configure_secret_key(app.env().get("SECRET_MASTER_KEY").value_or(""));
        service::config::configure_auth(service::config::get_auth_configuration(app.env()));
        auto initial_admin = service::initial_admin::get_initial_admin_config(app.env());
        configure_document_root(app, runtime_dir);
        auto database = configure_database(app);
        service::initial_admin::initialize(database, std::move(initial_admin));
        (void)configure_redis(app);
        service::config::configure_rate_limits(service::config::get_rate_limit_config(app.env()));
        service::geoip::configure_xdb_database(service::config::get_geoip_configuration(app.env()));
        app.use<service::middleware::request_rate_limit_middleware>();
        configure_worker_states(app, node_release);
        configure_outbound_origins(app, runtime_dir);
        configure_http_server(app, env_path.parent_path());

        app.on_start([] {
            const auto workers = ruvia::app().workers();
            if (workers.empty()) {
                throw std::runtime_error("service requires at least one Web Worker");
            }
            // Every instance receives exactly the same assembly. A task claims
            // durable work; it never chooses a Worker or takes a connection.
            for (const auto& worker : workers) {
                const auto post = [&worker](auto operation) {
                    const auto result = worker.post(
                        [operation](ruvia::web_worker_context& context) -> ruvia::task<void> {
                            try {
                                co_await operation(context);
                                if (!context.get_stop_token().stop_requested()) {
                                    throw std::runtime_error("Web Worker task stopped unexpectedly");
                                }
                            } catch (const std::exception& error) {
                                if (!context.get_stop_token().stop_requested()) {
                                    service::logging::error("Web Worker task failed: " + std::string(error.what()));
                                    ruvia::app().stop();
                                    throw;
                                }
                            }
                        });
                    if (!result.accepted()) {
                        throw std::runtime_error("Web Worker task could not start");
                    }
                };
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    co_await service::live_resource::run_notification_reader<service::live_resource::resource_subscriptions,
                        service::node_dispatch::fanout::desired_state_waits, service::log_ingest::fanout::log_tails,
                        service::log_ingest::fanout::log_tail_recovery, service::node_channel::report_receipts>(context);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::provider_verification::run_worker(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::dns_sync::run_worker(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::certificate_issuance::run_worker(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::website_dns::run_worker(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::node_presence::run_worker(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::live_resource::outbox::run_worker<service::node_presence::resource_relay,
                        service::log_ingest::log_relay, service::node_channel::projection_refresh>(task);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    service::background::task_context task(context);
                    co_await service::log_ingest::run_worker(task, std::string(task.lease_owner()));
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    co_await service::node_channel::run_projection(context);
                });
                post([](ruvia::web_worker_context& context) -> ruvia::task<void> {
                    co_await service::node_channel::run_reports(context);
                });
            }
        });
        app.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Server failed: " << ex.what() << '\n';
        return 1;
    }
}
