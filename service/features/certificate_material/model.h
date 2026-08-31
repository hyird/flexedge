#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

namespace service::certificate_material {

RUVIA_REQUEST_MODEL(
    CertificateMaterialInput,
    RUVIA_OPTIONAL_FIELD_NAME("certificate_chain_pem", certificateChainPem, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("private_key_envelope", privateKeyEnvelope, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("not_before", notBefore, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("serial_number", serialNumber, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("fingerprint_sha256", fingerprintSha256, ruvia::String),
    RUVIA_OPTIONAL_FIELD_NAME("last_issued_at", lastIssuedAt, ruvia::String));
RUVIA_RESPONSE_MODEL(
    CertificateMaterialOutput,
    RUVIA_REQUIRED_FIELD_NAME("certificate_chain_pem", certificateChainPem, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", privateKeyEnvelope, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("not_before", notBefore, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("serial_number", serialNumber, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("fingerprint_sha256", fingerprintSha256, ruvia::String),
    RUVIA_REQUIRED_FIELD_NAME("last_issued_at", lastIssuedAt, ruvia::String));

struct CertificateMaterialData final {
    std::optional<std::string> certificateChainPem;
    std::optional<std::string> privateKeyEnvelope;
    std::optional<std::string> notBefore;
    std::optional<std::string> serialNumber;
    std::optional<std::string> fingerprintSha256;
    std::optional<std::string> lastIssuedAt;
};

[[nodiscard]] inline CertificateMaterialData normalize(const CertificateMaterialInput& input) {
    CertificateMaterialData result;
    if (const auto& value = input.get<"certificateChainPem">()) {
        result.certificateChainPem = std::string(value->view());
    }
    if (const auto& value = input.get<"privateKeyEnvelope">()) {
        result.privateKeyEnvelope = std::string(value->view());
    }
    if (const auto& value = input.get<"notBefore">()) {
        result.notBefore = std::string(value->view());
    }
    if (const auto& value = input.get<"serialNumber">()) {
        result.serialNumber = std::string(value->view());
    }
    if (const auto& value = input.get<"fingerprintSha256">()) {
        result.fingerprintSha256 = std::string(value->view());
    }
    if (const auto& value = input.get<"lastIssuedAt">()) {
        result.lastIssuedAt = std::string(value->view());
    }
    return result;
}

[[nodiscard]] inline std::optional<CertificateMaterialData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<CertificateMaterialInput> input =
        ruvia::fromJson<CertificateMaterialInput>(json, options);
    return input ? std::optional<CertificateMaterialData>{normalize(*input)} : std::nullopt;
}

} // namespace service::certificate_material
