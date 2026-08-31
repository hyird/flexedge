#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <ruvia/web/Model.h>
#include <ruvia/web/ModelJson.h>

#include "service/features/certificate/model.h"
#include "service/features/certificate/provider_config.h"
#include "service/features/dns/provider_config.h"

namespace service::provider_verification {

RUVIA_REQUEST_MODEL(DnsProviderSnapshotInput, RUVIA_OPTIONAL_FIELD(provider, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(name, ruvia::String),
                    RUVIA_OPTIONAL_FIELD_NAME("account_id", accountId, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(revision, ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD_NAME("verification_generation", verificationGeneration,
                                              ruvia::Int64),
                    RUVIA_OPTIONAL_FIELD(config, service::dns::DnsProviderConfigInput));
RUVIA_RESPONSE_MODEL(DnsProviderSnapshotOutput, RUVIA_REQUIRED_FIELD(provider, ruvia::String),
                     RUVIA_REQUIRED_FIELD(name, ruvia::String),
                     RUVIA_REQUIRED_FIELD_NAME("account_id", accountId, ruvia::String),
                     RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
                     RUVIA_REQUIRED_FIELD_NAME("verification_generation", verificationGeneration,
                                               ruvia::Int64),
                     RUVIA_REQUIRED_FIELD(config, service::dns::DnsProviderConfigOutput));

RUVIA_REQUEST_MODEL(
    CertificateProviderSnapshotInput, RUVIA_OPTIONAL_FIELD(provider, ruvia::String),
    RUVIA_OPTIONAL_FIELD(revision, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD_NAME("verification_generation", verificationGeneration, ruvia::Int64),
    RUVIA_OPTIONAL_FIELD(config, service::certificate_issuance::CertificateProviderConfigInput));
RUVIA_RESPONSE_MODEL(
    CertificateProviderSnapshotOutput, RUVIA_REQUIRED_FIELD(provider, ruvia::String),
    RUVIA_REQUIRED_FIELD(revision, ruvia::Int64),
    RUVIA_REQUIRED_FIELD_NAME("verification_generation", verificationGeneration, ruvia::Int64),
    RUVIA_REQUIRED_FIELD(config, service::certificate_issuance::CertificateProviderConfigOutput));

RUVIA_REQUEST_MODEL(ProviderVerificationSnapshotInput, RUVIA_OPTIONAL_FIELD(kind, ruvia::String),
                    RUVIA_OPTIONAL_FIELD(dns, DnsProviderSnapshotInput),
                    RUVIA_OPTIONAL_FIELD(certificate, CertificateProviderSnapshotInput));
RUVIA_RESPONSE_MODEL(ProviderVerificationSnapshotOutput, RUVIA_REQUIRED_FIELD(kind, ruvia::String),
                     RUVIA_OPTIONAL_FIELD(dns, DnsProviderSnapshotOutput, RUVIA_OMIT_EMPTY),
                     RUVIA_OPTIONAL_FIELD(certificate, CertificateProviderSnapshotOutput,
                                          RUVIA_OMIT_EMPTY));

enum class ProviderVerificationKind {
    dns,
    certificate,
};

struct DnsProviderSnapshotData final {
    std::string provider;
    std::string name;
    std::string accountId;
    std::int64_t revision;
    std::int64_t verificationGeneration;
    service::dns::DnsProviderConfigData config;
};

struct CertificateProviderSnapshotData final {
    std::string provider;
    std::int64_t revision;
    std::int64_t verificationGeneration;
    service::certificate_issuance::CertificateProviderConfigData config;
};

[[nodiscard]] inline std::optional<DnsProviderSnapshotData>
normalize(const DnsProviderSnapshotInput& snapshot) {
    const auto& provider = snapshot.get<"provider">();
    const auto& name = snapshot.get<"name">();
    const auto& accountId = snapshot.get<"accountId">();
    const auto& revision = snapshot.get<"revision">();
    const auto& verificationGeneration = snapshot.get<"verificationGeneration">();
    const auto& config = snapshot.get<"config">();
    if (!provider || !name || !accountId || !revision || !verificationGeneration || !config) {
        return std::nullopt;
    }
    const auto normalizedConfig = service::dns::normalize(*config);
    if (!normalizedConfig) {
        return std::nullopt;
    }
    return DnsProviderSnapshotData{
        .provider = std::string(provider->view()),
        .name = std::string(name->view()),
        .accountId = std::string(accountId->view()),
        .revision = revision->value,
        .verificationGeneration = verificationGeneration->value,
        .config = *normalizedConfig,
    };
}

[[nodiscard]] inline std::optional<CertificateProviderSnapshotData>
normalize(const CertificateProviderSnapshotInput& snapshot) {
    const auto& provider = snapshot.get<"provider">();
    const auto& revision = snapshot.get<"revision">();
    const auto& verificationGeneration = snapshot.get<"verificationGeneration">();
    const auto& config = snapshot.get<"config">();
    if (!provider || !revision || !verificationGeneration || !config) {
        return std::nullopt;
    }
    const auto normalizedConfig = service::certificate_issuance::normalize(*config);
    if (!normalizedConfig) {
        return std::nullopt;
    }
    if (normalizedConfig->credentialMode == "email") {
        if (!normalizedConfig->accountEmail || normalizedConfig->envelope) {
            return std::nullopt;
        }
    } else if (normalizedConfig->credentialMode == "access_key") {
        if (normalizedConfig->accountEmail || !normalizedConfig->envelope ||
            !normalizedConfig->hint) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }
    return CertificateProviderSnapshotData{
        .provider = std::string(provider->view()),
        .revision = revision->value,
        .verificationGeneration = verificationGeneration->value,
        .config = *normalizedConfig,
    };
}

inline bool complete(const DnsProviderSnapshotInput& snapshot) {
    return normalize(snapshot).has_value();
}

inline bool complete(const CertificateProviderSnapshotInput& snapshot) {
    return normalize(snapshot).has_value();
}

struct ProviderVerificationSnapshotData final {
    using Value = std::variant<DnsProviderSnapshotData, CertificateProviderSnapshotData>;

    ProviderVerificationKind kind;
    Value value;
    std::int64_t revision;
    std::int64_t verificationGeneration;

    [[nodiscard]] bool isDns() const noexcept { return kind == ProviderVerificationKind::dns; }

    [[nodiscard]] std::string_view kindToken() const noexcept {
        return isDns() ? std::string_view{"dns"} : std::string_view{"certificate"};
    }

    [[nodiscard]] const DnsProviderSnapshotData& dns() const {
        return std::get<DnsProviderSnapshotData>(value);
    }

    [[nodiscard]] const CertificateProviderSnapshotData& certificate() const {
        return std::get<CertificateProviderSnapshotData>(value);
    }
};

[[nodiscard]] inline std::optional<ProviderVerificationSnapshotData>
normalize(const ProviderVerificationSnapshotInput& snapshot) {
    const auto& kind = snapshot.get<"kind">();
    const auto& dns = snapshot.get<"dns">();
    const auto& certificate = snapshot.get<"certificate">();
    if (!kind) {
        return std::nullopt;
    }
    if (kind->view() == "dns") {
        if (!dns || certificate) {
            return std::nullopt;
        }
        auto normalized = normalize(*dns);
        if (!normalized) {
            return std::nullopt;
        }
        const auto revision = normalized->revision;
        const auto verificationGeneration = normalized->verificationGeneration;
        return ProviderVerificationSnapshotData{
            .kind = ProviderVerificationKind::dns,
            .value = std::move(*normalized),
            .revision = revision,
            .verificationGeneration = verificationGeneration,
        };
    }
    if (kind->view() == "certificate") {
        if (!certificate || dns) {
            return std::nullopt;
        }
        auto normalized = normalize(*certificate);
        if (!normalized) {
            return std::nullopt;
        }
        const auto revision = normalized->revision;
        const auto verificationGeneration = normalized->verificationGeneration;
        return ProviderVerificationSnapshotData{
            .kind = ProviderVerificationKind::certificate,
            .value = std::move(*normalized),
            .revision = revision,
            .verificationGeneration = verificationGeneration,
        };
    }
    return std::nullopt;
}

inline bool complete(const ProviderVerificationSnapshotInput& snapshot) {
    return normalize(snapshot).has_value();
}

[[nodiscard]] inline std::optional<ProviderVerificationSnapshotData>
parseStored(std::string_view json, ruvia::ModelParseOptions options = {}) {
    const std::optional<ProviderVerificationSnapshotInput> input =
        ruvia::fromJson<ProviderVerificationSnapshotInput>(json, options);
    return input ? normalize(*input) : std::nullopt;
}

[[nodiscard]] inline DnsProviderSnapshotOutput toOutput(const DnsProviderSnapshotData& input,
                                                        ruvia::ModelOptions options = {}) {
    DnsProviderSnapshotOutput output(options);
    output.set<"provider">(input.provider);
    output.set<"name">(input.name);
    output.set<"accountId">(input.accountId);
    output.set<"revision">(input.revision);
    output.set<"verificationGeneration">(input.verificationGeneration);
    output.set<"config">(service::dns::toOutput(input.config, options));
    return output;
}

[[nodiscard]] inline CertificateProviderSnapshotOutput
toOutput(const CertificateProviderSnapshotData& input, ruvia::ModelOptions options = {}) {
    CertificateProviderSnapshotOutput output(options);
    output.set<"provider">(input.provider);
    output.set<"revision">(input.revision);
    output.set<"verificationGeneration">(input.verificationGeneration);
    output.set<"config">(service::certificate_issuance::toOutput(input.config, options));
    return output;
}

[[nodiscard]] inline ProviderVerificationSnapshotOutput
toOutput(const ProviderVerificationSnapshotData& input, ruvia::ModelOptions options = {}) {
    ProviderVerificationSnapshotOutput output(options);
    output.set<"kind">(input.kindToken());
    if (input.isDns()) {
        output.set<"dns">(toOutput(input.dns(), options));
    } else {
        output.set<"certificate">(toOutput(input.certificate(), options));
    }
    return output;
}

} // namespace service::provider_verification
