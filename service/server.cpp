#include <chrono>
#include <cstdio>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
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
#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>
#endif

#include <openssl/pem.h>
#include <openssl/x509.h>

#include <ruvia/core/Task.h>
#include <ruvia/http/HttpStatus.h>
#include <ruvia/web/App.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/Error.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbMigration.h>

#include "service/common/http.h"
#include "service/config/database.h"
#include "service/config/http_server.h"
#include "service/config/outbound.h"
#include "service/config/redis.h"
#include "service/config/runtime_env.h"
#include "service/config/schema.h"
#include "service/features/logging/access.h"
#include "service/features/logging/connection.h"
#include "service/features/logging/logger.h"

// 业务控制器（RUVIA_CONTROLLER_GROUP 在静态阶段把路由表注册到 ruvia::app()）。
#include "service/domains/website/website.controller.h"
#include "service/domains/auth/auth.controller.h"
#include "service/domains/agent/agent.controller.h"
#include "service/domains/certificate/certificate.controller.h"
#include "service/domains/cluster/cluster.controller.h"
#include "service/domains/dns_zone/dns_zone.controller.h"
#include "service/domains/node/node.controller.h"
#include "service/domains/overview/overview.controller.h"
#include "service/domains/provider/provider.controller.h"
#include "service/domains/task/task.controller.h"
#include "service/features/background/worker_pool.h"
#include "service/features/certificate/worker.h"
#include "service/features/dns_sync/worker.h"
#include "service/features/initial_admin/bootstrap.h"
#include "service/features/log_ingest/fanout.h"
#include "service/features/log_ingest/ingest.h"
#include "service/features/node_release/artifact.h"
#include "service/features/website_dispatch/worker.h"
#include "service/features/provider_verification/worker.h"
#include "service/utils/auth_session.h"
#include "service/utils/secret.h"
#include "service/utils/token.h"

namespace {

std::filesystem::path executableDir(const char* executablePath) {
    if (executablePath == nullptr || executablePath[0] == '\0') {
        return std::filesystem::current_path();
    }

    std::filesystem::path path(executablePath);
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

void logMigrationReport(const ruvia::DbMigrationReport& report) {
    service::logging::info("DB migrations applied=" + std::to_string(report.applied().size()) +
                           ", skipped=" + std::to_string(report.skipped().size()));
}

ruvia::DbConfig configureDatabase(ruvia::App& app) {
    auto dbConfig = service::config::databaseConfig(app.env());
    const ruvia::DbMigratorOptions migrationOptions{.table = "schema_migrations"};
    logMigrationReport(
        ruvia::DbMigrator::migrate(dbConfig, service::config::kSchemaMigrations, migrationOptions));
    app.database({.alias = "default", .config = dbConfig});
    return dbConfig;
}

void configureRedis(ruvia::App& app) {
    app.redis({.config = service::config::redisConfig(app.env())});
}

std::filesystem::path systemCaBundle([[maybe_unused]] const std::filesystem::path& runtimeDir) {
#ifdef _WIN32
    const auto path = runtimeDir / "system-ca-bundle.pem";
    FILE* output = nullptr;
    if (_wfopen_s(&output, path.c_str(), L"wb") != 0 || output == nullptr)
        throw std::runtime_error("failed to create Windows system CA bundle");

    auto* bio = BIO_new_fp(output, BIO_NOCLOSE);
    if (bio == nullptr) {
        std::fclose(output);
        throw std::runtime_error("failed to initialize Windows system CA bundle");
    }

    std::size_t certificateCount = 0;
    const auto appendStore = [&](DWORD location) {
        const auto store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                                         location | CERT_STORE_OPEN_EXISTING_FLAG, L"ROOT");
        if (store == nullptr)
            return;
        PCCERT_CONTEXT context = nullptr;
        while ((context = CertEnumCertificatesInStore(store, context)) != nullptr) {
            const unsigned char* encoded = context->pbCertEncoded;
            auto* certificate = d2i_X509(nullptr, &encoded, context->cbCertEncoded);
            if (certificate == nullptr)
                continue;
            if (PEM_write_bio_X509(bio, certificate) == 1)
                ++certificateCount;
            X509_free(certificate);
        }
        CertCloseStore(store, 0);
    };
    appendStore(CERT_SYSTEM_STORE_LOCAL_MACHINE);
    appendStore(CERT_SYSTEM_STORE_CURRENT_USER);
    BIO_free(bio);
    std::fclose(output);
    if (certificateCount == 0)
        throw std::runtime_error("Windows system CA store is empty");
    return path;
#else
    return {};
#endif
}

void configureOutboundOrigins(ruvia::App& app, const std::filesystem::path& runtimeDir) {
    const auto caFile = systemCaBundle(runtimeDir).string();
    auto origins = service::config::makeOutboundOrigins(caFile);
    app.httpClient({.alias = std::string(service::config::kCloudflareOriginAlias),
                    .config = origins.cloudflare})
        .httpClient({.alias = std::string(service::config::kAliyunDnsOriginAlias),
                     .config = origins.aliyunDns})
        .httpClient({.alias = std::string(service::config::kLetsEncryptOriginAlias),
                     .config = origins.letsEncrypt})
        .httpClient({.alias = std::string(service::config::kZeroSslAcmeOriginAlias),
                     .config = origins.zeroSslAcme})
        .httpClient({.alias = std::string(service::config::kZeroSslApiOriginAlias),
                     .config = origins.zeroSslApi})
        .httpClient({.alias = std::string(service::config::kAliDnsDohOriginAlias),
                     .config = origins.aliDnsDoh});
    service::config::configureOutboundOrigins(std::move(origins));
}

ruvia::Task<ruvia::HttpResponse> handleNotFound(ruvia::Context& c) {
    c.status(ruvia::http_status::kNotFound);
    co_return c.json(service::common::error(
        c, service::common::normalizeBusinessErrorCode({}, ruvia::http_status::kNotFound.value()),
        ruvia::httpReasonPhrase(ruvia::http_status::kNotFound)));
}

// 与 ruvia::HttpErrorHandler 签名匹配；运行时把业务 HttpError 转成统一响应 DTO。
ruvia::Task<ruvia::HttpResponse> handleError(ruvia::Context& c, ruvia::HttpErrorInfo info) {
    const auto status = info.status();
    if (status.isServerError()) {
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
            diagnostic = info.message().empty() ? std::string(ruvia::httpReasonPhrase(status))
                                                : std::string(info.message());
        }
        service::logging::error("Unhandled error: " + diagnostic);
    }
    c.status(status);
    co_return c.json(service::common::error(
        c, service::common::normalizeBusinessErrorCode(info.code(), status.value()),
        service::common::responseErrorMessage(info)));
}

void configureHttpServer(ruvia::App& app) {
    const auto settings = service::config::httpServerSettings(app.env());
    app.blockingPool({
                         .threadCount = settings.blockingThreads,
                         .queueCapacity = settings.blockingQueueCapacity,
                     })
        .compression({.minBytes = settings.compressionMinBytes})
        .listen({.address = settings.host, .http = settings.port})
        .server({.workerCount = settings.workerCount,
                 .processSignalHandlers = ruvia::ProcessSignalHandlerPolicy::kInstall})
        .onNotFound(&handleNotFound)
        .onError(&handleError)
        .onAccess(ruvia::AccessLogCallback(service::logging::AccessLogger{}))
        .onConnectionFailure(
            ruvia::ConnectionFailureCallback(service::logging::ConnectionFailureLogger{}));
    service::logging::info("Server starting on " + settings.host + ":" +
                           std::to_string(settings.port));
}

} // namespace

// 健康检查。
class HealthController final : public ruvia::Controller<HealthController> {
  public:
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/api/health", health);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> health(ruvia::Context& c) {
        co_return c.json(service::common::health(c));
    }
};

int main(int argc, char* argv[]) {
    try {
        auto& app = ruvia::app();
        service::logging::Session loggingSession;
        const auto runtimeDir = executableDir(argc > 0 ? argv[0] : nullptr);
        service::node_release::configure(runtimeDir / "node", runtimeDir / "install-node.sh",
                                         runtimeDir / "node-release.manifest");
        const auto envPath = service::config::dotenvPath(runtimeDir);
        app.loadDotenv(envPath, {.missingFile = ruvia::DotenvMissingFilePolicy::kRequire});
        if (service::config::ensureRuntimeSecrets(envPath, app.env())) {
            app.loadDotenv(envPath);
        }
        service::utils::configureSecretKey(app.env().get("SECRET_MASTER_KEY").value_or(""));
        service::auth::validateAuthSessionConfiguration();
        auto initialAdmin = service::initial_admin::initialAdminConfig(app.env());
        auto database = configureDatabase(app);
        service::initial_admin::initialize(database, std::move(initialAdmin));
        configureRedis(app);
        configureOutboundOrigins(app, runtimeDir);
        configureHttpServer(app);

        std::vector<service::background::WorkerDefinition> workers;
        workers.emplace_back(service::background::WorkerDefinition{
            .name = "provider-verification",
            .outboundAliases = {service::config::kCloudflareOriginAlias,
                                service::config::kAliyunDnsOriginAlias,
                                service::config::kLetsEncryptOriginAlias,
                                service::config::kZeroSslAcmeOriginAlias,
                                service::config::kZeroSslApiOriginAlias},
            .run = [](service::background::WorkerContext& context) -> ruvia::Task<void> {
                co_await service::provider_verification::runWorker(context);
            },
        });
        workers.emplace_back(service::background::WorkerDefinition{
            .name = "dns-sync",
            .outboundAliases = {service::config::kCloudflareOriginAlias,
                                service::config::kAliyunDnsOriginAlias},
            .run = [](service::background::WorkerContext& context) -> ruvia::Task<void> {
                co_await service::dns_sync::runWorker(context);
            },
        });
        workers.emplace_back(service::background::WorkerDefinition{
            .name = "certificate",
            .outboundAliases = {service::config::kCloudflareOriginAlias,
                                service::config::kAliyunDnsOriginAlias,
                                service::config::kLetsEncryptOriginAlias,
                                service::config::kZeroSslAcmeOriginAlias},
            .run = [](service::background::WorkerContext& context) -> ruvia::Task<void> {
                co_await service::certificate_issuance::runWorker(context);
            },
        });
        workers.emplace_back(service::background::WorkerDefinition{
            .name = "website-dispatch",
            .outboundAliases = {service::config::kAliDnsDohOriginAlias},
            .run = [](service::background::WorkerContext& context) -> ruvia::Task<void> {
                co_await service::website_dispatch::runWorker(context);
            },
        });
        service::background::WorkerPool backgroundWorkers(
            database, service::config::outboundOrigins(), std::move(workers));
        const auto logConsumerInstance = service::utils::randomToken().substr(0, 32);
        app.onStart([&backgroundWorkers, logConsumerInstance] {
               backgroundWorkers.start();
               auto logWorkers = ruvia::app().workers();
               if (logWorkers.empty()) {
                   throw std::runtime_error("log fanout requires a web worker");
               }
               const auto fanoutPosted = logWorkers.front().post(
                   [](ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
                       co_await service::log_ingest::fanout::run(context);
                   });
               if (!fanoutPosted.accepted()) {
                   throw std::runtime_error("log notification fanout could not start");
               }
               for (auto& worker : logWorkers) {
                   auto consumer = logConsumerInstance + "-" + std::to_string(worker.id());
                   const auto posted =
                       worker.post([consumer = std::move(consumer)](
                                       ruvia::WebWorkerContext& context) -> ruvia::Task<void> {
                           co_await service::log_ingest::runWorker(context, consumer);
                       });
                   if (!posted.accepted()) {
                       throw std::runtime_error("log ingest worker could not start");
                   }
               }
           })
            .onStop([&backgroundWorkers] { backgroundWorkers.stop(); });
        app.run();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Server failed: " << ex.what() << '\n';
        return 1;
    }
}
