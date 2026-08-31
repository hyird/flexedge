#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <zlib.h>

#include <ruvia/core/Task.h>
#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbTransaction.h>

#include "service/common/database.h"
#include "service/common/http.h"
#include "service/domains/certificate/certificate.error.h"
#include "service/domains/certificate/certificate.types.h"
#include "service/features/certificate/model.h"
#include "service/features/certificate/dns_challenge.h"
#include "service/features/certificate_material/model.h"
#include "service/features/certificate/queue.h"
#include "service/features/dns/registry.h"
#include "service/features/sync_runtime/state.h"
#include "service/utils/secret.h"
#include "service/utils/sensitive_string.h"

namespace service::certificate {

struct CertificateDownload final {
    std::string filename;
    std::string archive;
};

class CertificateService {
  public:
    ruvia::Task<CertificatePageDataDto>
    list(ruvia::Context& c, const std::string& tenantId, std::int64_t page, std::int64_t pageSize,
         std::int64_t skip, const std::optional<std::string>& keyword,
         std::optional<std::string_view> status, std::optional<bool> usable) {
        std::string where =
            " FROM sys_certificate cert INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
            "cert.tenant_id AND zone.id = cert.dns_zone_id INNER JOIN sys_provider provider ON "
            "provider.tenant_id = cert.tenant_id AND provider.id = cert.provider_id AND "
            "provider.kind = 'certificate' LEFT JOIN LATERAL (SELECT task.operation, "
            "CASE WHEN task.lease_until IS NOT NULL THEN 'running' WHEN task.is_done AND "
            "task.is_ok THEN 'completed' WHEN task.count_fails > 0 THEN 'retry' ELSE 'pending' END "
            "AS sync_status, task.count_fails AS sync_count_fails FROM sys_sync_task task WHERE "
            "task.resource_type = 'certificate' AND "
            "task.tenant_id = cert.tenant_id AND task.resource_id = cert.id AND task.version = "
            "cert.issuance_revision ORDER BY task.updated_at DESC LIMIT 1) latest_task ON TRUE "
            "WHERE cert.deleted_at IS NULL AND "
            "cert.tenant_id = $1";
        std::vector<ruvia::DbValue> params{ruvia::DbValue{tenantId}};
        std::optional<std::string> pattern;
        if (keyword) {
            pattern = "%" + service::common::escapeLikePattern(*keyword) + "%";
            where += " AND cert.domain ILIKE $" + std::to_string(params.size() + 1);
            params.emplace_back(std::string_view(*pattern));
        }
        if (status && !status->empty()) {
            where += " AND cert.status = $" + std::to_string(params.size() + 1);
            params.emplace_back(*status);
        }
        if (usable) {
            where += " AND COALESCE(cert.issued_revision > 0 AND cert.expires_at > NOW(), FALSE) "
                     "= $" +
                     std::to_string(params.size() + 1);
            params.emplace_back(*usable);
        }
        const auto countRows = co_await c.db().query("SELECT COUNT(*)" + where, params);
        const auto total = countRows.empty() ? std::int64_t{0}
                                             : countRows.front()[0].as<std::int64_t>().value_or(0);
        const auto rows =
            co_await c.db().query(certificateColumns() + where + " ORDER BY cert.sort DESC LIMIT " +
                                      std::to_string(pageSize) + " OFFSET " + std::to_string(skip),
                                  params);
        CertificatePageDataDto result(c);
        result.set<"total">(total);
        result.set<"page">(page);
        result.set<"pageSize">(pageSize);
        result.set<"totalPages">(pageSize > 0 ? (total + pageSize - 1) / pageSize : 0);
        auto& items = result.ensure<"list">();
        for (const auto& row : rows) {
            fillCertificate(c, items.emplace_back(c), row);
        }
        co_return result;
    }

    ruvia::Task<CertificateDto> get(ruvia::Context& c, const std::string& tenantId,
                                    const std::string& id) {
        const auto rows = co_await c.db().query(
            certificateColumns() +
                " FROM sys_certificate cert INNER JOIN sys_dns_zone zone ON zone.tenant_id = "
                "cert.tenant_id AND zone.id = cert.dns_zone_id INNER JOIN sys_provider "
                "provider ON provider.tenant_id = cert.tenant_id AND provider.id = "
                "cert.provider_id AND provider.kind = 'certificate' LEFT JOIN LATERAL (SELECT "
                "task.operation, CASE WHEN task.lease_until IS NOT NULL THEN 'running' WHEN "
                "task.is_done AND task.is_ok THEN 'completed' WHEN task.count_fails > 0 THEN "
                "'retry' "
                "ELSE 'pending' END AS sync_status, task.count_fails AS sync_count_fails FROM "
                "sys_sync_task task "
                "WHERE "
                "task.resource_type = 'certificate' AND task.tenant_id = cert.tenant_id AND "
                "task.resource_id = cert.id AND task.version = cert.issuance_revision ORDER BY "
                "task.updated_at DESC LIMIT 1) latest_task ON TRUE WHERE cert.id = $1 AND "
                "cert.tenant_id = $2 AND "
                "cert.deleted_at IS NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        CertificateDto result(c);
        fillCertificate(c, result, rows.front());
        co_return result;
    }

    ruvia::Task<void> create(ruvia::Context& c, const std::string& tenantId,
                             const CreateCertificateBody& body) {
        const auto& domainInput = body.get<"domain">();
        const auto& providerIdInput = body.get<"certificateProviderId">();
        const auto& zoneIdInput = body.get<"dnsZoneId">();
        const auto& configInput = body.get<"config">();
        if (!domainInput || !providerIdInput || !zoneIdInput || !configInput) {
            service::common::throwAppError(service::common::kValidationErrorCode,
                                           "证书域名、供应商、托管域名和配置不能为空", 400);
        }
        const auto config = service::certificate_issuance::normalize(*configInput);
        if (!config) {
            service::common::throwAppError(service::common::kValidationErrorCode, "证书配置不正确",
                                           400);
        }
        const auto domain = normalizeDomain(domainInput->view());
        const auto providerId = std::string(providerIdInput->view());
        const auto zoneId = std::string(zoneIdInput->view());
        auto transaction = co_await c.db().beginTransaction();
        const auto exists = co_await transaction.query(
            "SELECT id FROM sys_certificate WHERE tenant_id = $1 AND domain = $2 AND "
            "deleted_at IS NULL LIMIT 1",
            tenantId, std::string_view(domain));
        if (!exists.empty()) {
            service::common::throwAppError(CertificateError::DOMAIN_EXISTS);
        }
        const auto providerRows = co_await transaction.query(
            "SELECT id FROM sys_provider WHERE id = $1 AND tenant_id = $2 AND kind = "
            "'certificate' AND status = 'verified' AND deleted_at IS NULL LIMIT 1 FOR SHARE",
            providerId, tenantId);
        if (providerRows.empty()) {
            service::common::throwAppError(CertificateError::PROVIDER_UNAVAILABLE);
        }
        const auto zoneRows = co_await transaction.query(
            "SELECT zone.domain, dns_provider.provider FROM sys_dns_zone zone INNER JOIN "
            "sys_provider dns_provider ON dns_provider.tenant_id = zone.tenant_id AND "
            "dns_provider.id = zone.provider_id AND dns_provider.kind = 'dns' WHERE zone.id = $1 "
            "AND zone.tenant_id = $2 AND zone.sync_status = 'synced' AND "
            "dns_provider.status = 'verified' AND zone.deleted_at IS NULL AND "
            "dns_provider.deleted_at IS NULL LIMIT 1 FOR SHARE OF zone, dns_provider",
            zoneId, tenantId);
        if (zoneRows.empty()) {
            service::common::throwAppError(CertificateError::DOMAIN_UNAVAILABLE);
        }
        if (!service::dns::findDnsProvider(zoneRows.front()[1].value().value_or(""))) {
            service::common::throwAppError(CertificateError::DNS_PROVIDER_UNSUPPORTED);
        }
        const auto zoneDomain = std::string(zoneRows.front()[0].value().value_or(""));
        const auto plainDomain = domain.starts_with("*.") ? domain.substr(2) : domain;
        if (plainDomain != zoneDomain &&
            (plainDomain.size() <= zoneDomain.size() || !plainDomain.ends_with("." + zoneDomain))) {
            service::common::throwAppError(CertificateError::DOMAIN_MISMATCH);
        }
        const auto configJson = serializeCertificateConfig(c, *config);
        std::string certificateId;
        try {
            const auto saved = co_await transaction.query(
                "INSERT INTO sys_certificate (tenant_id, provider_id, domain, dns_zone_id, "
                "status, config, material, revision, issuance_revision, issued_revision, "
                "created_at, updated_at) VALUES ($1, $2, $3, $4, 'pending', $5::jsonb, "
                "'{}'::jsonb, 1, 1, 0, NOW(), NOW()) RETURNING id",
                tenantId, providerId, std::string_view(domain), zoneId,
                std::string_view(configJson));
            certificateId = std::string(saved.front()[0].value().value_or(""));
        } catch (const ruvia::DbError& error) {
            if (service::common::isUniqueConstraintViolation(error, "uk_certificate_domain")) {
                service::common::throwAppError(CertificateError::DOMAIN_EXISTS);
            }
            throw;
        }
        (void)co_await service::certificate_issuance::enqueueCertificateRevision(
            transaction, tenantId, certificateId, 1, "issue");
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> update(
        ruvia::Context& c, const std::string& tenantId, const std::string& id,
        std::int64_t expectedRevision,
        const ruvia::ValidatedJson<service::certificate_issuance::CertificateConfigInput>& config) {
        auto transaction = co_await c.db().beginTransaction();
        const auto certificates = co_await transaction.query(
            "SELECT revision FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND "
            "deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (certificates.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto normalizedConfig = service::certificate_issuance::normalize(config.value());
        if (!normalizedConfig) {
            service::common::throwAppError(service::common::kValidationErrorCode, "证书配置不正确",
                                           400);
        }
        const auto configJson = serializeCertificateConfig(c, *normalizedConfig);
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET revision = revision + 1, config = $1::jsonb, updated_at = "
            "NOW() WHERE id = $2 AND tenant_id = $3 AND revision = $4 AND deleted_at IS NULL",
            std::string_view(configJson), id, tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> renew(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                            std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto rows = co_await transaction.query(
            "SELECT revision, issuance_revision, issued_revision FROM sys_certificate WHERE id = "
            "$1 AND tenant_id = $2 AND deleted_at IS NULL LIMIT 1 FOR UPDATE",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (rows.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto issuanceRevision = rows.front()[1].as<std::int64_t>().value_or(0) + 1;
        const auto hasCertificate = rows.front()[2].as<std::int64_t>().value_or(0) > 0;
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET issuance_revision = $1, status = $2, last_error = NULL, "
            "updated_at = NOW() WHERE id = $3 AND tenant_id = $4 AND revision = $5 AND "
            "deleted_at IS NULL",
            issuanceRevision,
            hasCertificate ? std::string_view{"renewing"} : std::string_view{"pending"}, id,
            tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        (void)co_await service::certificate_issuance::enqueueCertificateRevision(
            transaction, tenantId, id, issuanceRevision, "renew");
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<void> remove(ruvia::Context& c, const std::string& tenantId, const std::string& id,
                             std::int64_t expectedRevision) {
        auto transaction = co_await c.db().beginTransaction();
        const auto certificates =
            co_await transaction.query("SELECT revision, dns_zone_id FROM sys_certificate WHERE id "
                                       "= $1 AND tenant_id = $2 AND "
                                       "deleted_at IS NULL LIMIT 1 FOR UPDATE",
                                       id, tenantId);
        if (certificates.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (certificates.front()[0].as<std::int64_t>().value_or(0) != expectedRevision) {
            service::common::throwAppError(CertificateError::REVISION_CONFLICT);
        }
        const auto dnsZoneId = certificates.front()[1].value().value_or("");
        const auto consumers = co_await transaction.query(
            "SELECT website_id FROM sys_website_certificate_binding WHERE tenant_id = $1 AND "
            "certificate_id = $2 LIMIT 1",
            tenantId, id);
        if (!consumers.empty()) {
            service::common::throwAppError(CertificateError::IN_USE);
        }
        const auto result = co_await transaction.execute(
            "UPDATE sys_certificate SET material = '{}'::jsonb, revision = revision + 1, "
            "deleted_at = NOW(), updated_at = NOW() WHERE id = $1 AND tenant_id = $2 AND "
            "revision = $3 AND deleted_at IS NULL",
            id, tenantId, expectedRevision);
        if (result.affectedRows() == 0) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        co_await service::certificate_issuance::retireDnsChallengesForDeletedCertificate(
            transaction, tenantId, dnsZoneId, id);
        co_await service::sync_runtime::removeMarker(transaction, tenantId, "certificate", id);
        co_await transaction.commit();
        co_return;
    }

    ruvia::Task<CertificateDownload> download(ruvia::Context& c, const std::string& tenantId,
                                              const std::string& id) {
        const auto rows = co_await c.db().query(
            "SELECT domain, material::text, COALESCE(issued_revision > 0 AND expires_at > NOW(), "
            "FALSE) FROM sys_certificate WHERE id = $1 AND tenant_id = $2 AND deleted_at IS "
            "NULL LIMIT 1",
            id, tenantId);
        if (rows.empty()) {
            service::common::throwAppError(CertificateError::NOT_FOUND);
        }
        if (!rows.front()[2].as<bool>().value_or(false)) {
            service::common::throwAppError(CertificateError::CERTIFICATE_UNAVAILABLE);
        }
        const auto material = service::certificate_material::parseStored(
            rows.front()[1].value().value_or("{}"), {.resource = c.resource()});
        if (!material) {
            throw std::runtime_error("stored certificate material is invalid");
        }
        if (!material->certificateChainPem || !material->privateKeyEnvelope) {
            service::common::throwAppError(CertificateError::CERTIFICATE_UNAVAILABLE);
        }
        const auto filename = certificateFilename(rows.front()[0].value().value_or("certificate"));
        auto archive = co_await c.runBlocking(
            [archiveFilename = filename, chain = std::move(*material->certificateChainPem),
             privateKey = service::utils::SensitiveString(
                 service::utils::openSecret(*material->privateKeyEnvelope))]() {
                return CertificateService::buildCertificateArchive(archiveFilename, chain,
                                                                   privateKey.view());
            });
        co_return CertificateDownload{
            filename + ".zip",
            std::move(archive),
        };
    }

  private:
    static std::string certificateColumns() {
        return "SELECT cert.id, cert.revision, cert.config::text, cert.status, "
               "cert.material::text, TO_CHAR(cert.expires_at, "
               "'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), cert.last_error, "
               "TO_CHAR(cert.created_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "TO_CHAR(cert.updated_at, 'YYYY-MM-DD\"T\"HH24:MI:SS.USOF'), "
               "cert.dns_zone_id, zone.domain, CASE WHEN cert.expires_at IS NULL THEN NULL ELSE "
               "GREATEST(0, FLOOR(EXTRACT(EPOCH FROM (cert.expires_at - NOW())) / 86400))::BIGINT "
               "END, cert.subject_alt_names[1], cert.subject_alt_names[2], provider.id, "
               "provider.provider, latest_task.sync_status, latest_task.sync_count_fails, "
               "(SELECT COUNT(DISTINCT website.id) FROM "
               "sys_website_certificate_binding binding INNER JOIN sys_website website ON "
               "website.id = binding.website_id AND website.tenant_id = cert.tenant_id "
               "AND website.deleted_at IS NULL WHERE binding.tenant_id = cert.tenant_id "
               "AND binding.certificate_id = cert.id), COALESCE(cert.issued_revision > 0 AND "
               "cert.expires_at > NOW(), FALSE)";
    }

    static std::string
    serializeCertificateConfig(ruvia::Context& c,
                               const service::certificate_issuance::CertificateConfigData& config) {
        const auto output =
            service::certificate_issuance::toOutput(config, {.resource = c.resource()});
        const auto json = ruvia::toJson(output, {.resource = c.resource()});
        return std::string(json.data(), json.size());
    }

    struct ZipEntry final {
        std::string filename;
        std::string compressed;
        std::uint32_t checksum;
        std::uint32_t uncompressedSize;
        std::uint32_t localHeaderOffset;
    };

    template <typename Value>
        requires std::is_unsigned_v<Value>
    static void appendLittleEndian(std::string& output, Value value) {
        for (std::size_t index = 0; index < sizeof(Value); ++index) {
            output.push_back(static_cast<char>(value & static_cast<Value>(0xff)));
            value >>= 8;
        }
    }

    static std::uint32_t zipSize(std::size_t value, std::string_view field) {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("certificate archive " + std::string(field) +
                                     " exceeds ZIP32 limits");
        }
        return static_cast<std::uint32_t>(value);
    }

    static std::uint16_t zipFilenameSize(std::size_t value) {
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("certificate archive filename exceeds ZIP limits");
        }
        return static_cast<std::uint16_t>(value);
    }

    static std::string deflateForZip(std::string_view input) {
        if (input.size() > std::numeric_limits<uInt>::max()) {
            throw std::runtime_error("certificate file exceeds deflate limits");
        }

        z_stream stream{};
        if (deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            throw std::runtime_error("failed to initialize certificate archive compression");
        }
        struct StreamGuard final {
            z_stream* stream;
            ~StreamGuard() { deflateEnd(stream); }
        } guard{&stream};

        const auto bound = deflateBound(&stream, static_cast<uLong>(input.size()));
        if (bound > std::numeric_limits<uInt>::max()) {
            throw std::runtime_error("compressed certificate file exceeds deflate limits");
        }
        std::string output(static_cast<std::size_t>(bound), '\0');
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());
        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
            throw std::runtime_error("failed to compress certificate archive entry");
        }
        output.resize(static_cast<std::size_t>(stream.total_out));
        return output;
    }

    static ZipEntry makeZipEntry(std::string filename, std::string_view content) {
        if (content.empty()) {
            throw std::runtime_error("certificate archive entry is empty");
        }
        const auto checksum = crc32_z(
            crc32(0L, Z_NULL, 0), reinterpret_cast<const Bytef*>(content.data()), content.size());
        return {
            .filename = std::move(filename),
            .compressed = deflateForZip(content),
            .checksum = static_cast<std::uint32_t>(checksum),
            .uncompressedSize = zipSize(content.size(), "entry"),
            .localHeaderOffset = 0,
        };
    }

    static void appendLocalHeader(std::string& archive, ZipEntry& entry) {
        entry.localHeaderOffset = zipSize(archive.size(), "offset");
        appendLittleEndian<std::uint32_t>(archive, 0x04034b50);
        appendLittleEndian<std::uint16_t>(archive, 20);
        appendLittleEndian<std::uint16_t>(archive, 0x0800);
        appendLittleEndian<std::uint16_t>(archive, 8);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0x0021);
        appendLittleEndian<std::uint32_t>(archive, entry.checksum);
        appendLittleEndian<std::uint32_t>(archive, zipSize(entry.compressed.size(), "entry"));
        appendLittleEndian<std::uint32_t>(archive, entry.uncompressedSize);
        appendLittleEndian<std::uint16_t>(archive, zipFilenameSize(entry.filename.size()));
        appendLittleEndian<std::uint16_t>(archive, 0);
        archive.append(entry.filename);
        archive.append(entry.compressed);
    }

    static void appendCentralHeader(std::string& archive, const ZipEntry& entry) {
        appendLittleEndian<std::uint32_t>(archive, 0x02014b50);
        appendLittleEndian<std::uint16_t>(archive, 20);
        appendLittleEndian<std::uint16_t>(archive, 20);
        appendLittleEndian<std::uint16_t>(archive, 0x0800);
        appendLittleEndian<std::uint16_t>(archive, 8);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0x0021);
        appendLittleEndian<std::uint32_t>(archive, entry.checksum);
        appendLittleEndian<std::uint32_t>(archive, zipSize(entry.compressed.size(), "entry"));
        appendLittleEndian<std::uint32_t>(archive, entry.uncompressedSize);
        appendLittleEndian<std::uint16_t>(archive, zipFilenameSize(entry.filename.size()));
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint32_t>(archive, 0);
        appendLittleEndian<std::uint32_t>(archive, entry.localHeaderOffset);
        archive.append(entry.filename);
    }

    static std::string buildCertificateArchive(std::string_view filename,
                                               std::string_view certificateChain,
                                               std::string_view privateKey) {
        std::array entries{
            makeZipEntry(std::string(filename) + ".crt", certificateChain),
            makeZipEntry(std::string(filename) + ".key", privateKey),
        };
        std::string archive;
        archive.reserve(certificateChain.size() + privateKey.size() + 256);
        for (auto& entry : entries) {
            appendLocalHeader(archive, entry);
        }

        const auto centralOffset = zipSize(archive.size(), "central directory offset");
        for (const auto& entry : entries) {
            appendCentralHeader(archive, entry);
        }
        const auto centralSize = zipSize(archive.size() - centralOffset, "central directory");
        appendLittleEndian<std::uint32_t>(archive, 0x06054b50);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, 0);
        appendLittleEndian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
        appendLittleEndian<std::uint16_t>(archive, static_cast<std::uint16_t>(entries.size()));
        appendLittleEndian<std::uint32_t>(archive, centralSize);
        appendLittleEndian<std::uint32_t>(archive, centralOffset);
        appendLittleEndian<std::uint16_t>(archive, 0);
        return archive;
    }

    static std::string certificateFilename(std::string_view domain) {
        std::string result;
        if (domain.starts_with("*.")) {
            domain.remove_prefix(2);
        }
        result.reserve(domain.size());
        for (const auto character : domain) {
            const auto value = static_cast<unsigned char>(character);
            result.push_back(
                std::isalnum(value) != 0 || character == '.' || character == '-' ? character : '_');
        }
        return result.empty() ? "certificate" : result;
    }

    static std::string normalizeDomain(std::string_view input) {
        std::string result(input);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return result;
    }

    template <typename Row>
    static void fillCertificate(ruvia::Context& c, CertificateDto& item, const Row& row) {
        const auto config = service::certificate_issuance::parseConfigStored(
            row[2].value().value_or("{}"), {.resource = c.resource()});
        if (!config) {
            throw std::runtime_error("stored certificate config is incomplete");
        }
        const auto material = service::certificate_material::parseStored(
            row[4].value().value_or("{}"), {.resource = c.resource()});
        if (!material) {
            throw std::runtime_error("stored certificate material is invalid");
        }
        auto configDto =
            service::certificate_issuance::toOutput(*config, {.resource = c.resource()});
        item.set<"id">(row[0].value().value_or(""));
        item.set<"revision">(row[1].template as<std::int64_t>().value_or(1));
        item.set<"issuer">(row[15].value().value_or("") == "zerossl" ? "ZeroSSL" : "Let's Encrypt");
        item.set<"status">(row[3].value().value_or(""));
        item.set<"config">(std::move(configDto));
        item.set<"createdAt">(row[7].value().value_or(""));
        item.set<"updatedAt">(row[8].value().value_or(""));
        item.set<"dnsZoneId">(row[9].value().value_or(""));
        item.set<"dnsZoneDomain">(row[10].value().value_or(""));
        item.set<"certificateProviderId">(row[14].value().value_or(""));
        item.set<"certificateProvider">(row[15].value().value_or(""));
        item.set<"websiteCount">(row[18].template as<std::int64_t>().value_or(0));
        item.set<"usable">(row[19].template as<bool>().value_or(false));
        if (material->notBefore) {
            item.set<"notBefore">(*material->notBefore);
        }
        if (const auto& expiresAt = row[5].value()) {
            item.set<"expiresAt">(*expiresAt);
        }
        if (const auto& lastError = row[6].value()) {
            item.set<"lastError">(*lastError);
        }
        if (row[11].value()) {
            item.set<"remainingDays">(row[11].template as<std::int64_t>().value_or(0));
        }
        if (material->serialNumber) {
            item.set<"serialNumber">(*material->serialNumber);
        }
        if (material->fingerprintSha256) {
            item.set<"fingerprintSha256">(*material->fingerprintSha256);
        }
        if (material->lastIssuedAt) {
            item.set<"lastIssuedAt">(*material->lastIssuedAt);
        }
        if (const auto& syncStatus = row[16].value()) {
            item.set<"syncStatus">(*syncStatus);
        }
        if (row[17].value()) {
            item.set<"syncCountFails">(row[17].template as<std::int64_t>().value_or(0));
        }
        auto& domains = item.ensure<"domains">();
        if (const auto& primaryDomain = row[12].value()) {
            domains.emplace_back(*primaryDomain, ruvia::ModelOptions{.resource = c.resource()});
        }
        if (const auto& secondaryDomain = row[13].value()) {
            domains.emplace_back(*secondaryDomain, ruvia::ModelOptions{.resource = c.resource()});
        }
    }
};

inline CertificateService& certificateService() {
    static CertificateService service;
    return service;
}

} // namespace service::certificate
