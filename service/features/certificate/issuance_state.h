#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace service::certificate_issuance {

// Where a resumable issuance stands. `validation`: the order's DNS-01
// challenges are published; the next attempt answers and polls them.
// `finalize`: the authorizations are valid; the next attempt finalizes the
// order with the stored certificate key and downloads the chain.
enum class issuance_phase {
    validation,
    finalize,
};

// An authorization of the order that still needs its DNS-01 challenge.
struct issuance_authorization final {
    std::string authorization_url_;
    std::string challenge_url_;
};

// The persisted progress of one issuance revision (sys_certificate.issuance_state).
// An attempt resumes it only for the same issuance revision and ACME account.
struct issuance_state final {
    std::int64_t revision_{};
    issuance_phase phase_{issuance_phase::validation};
    std::string account_url_;
    std::string order_url_;
    std::vector<issuance_authorization> authorizations_;
    // The zone revision containing this order's TXT records. It survives
    // resumption, which clears the sync marker's temporary wait reference.
    std::int64_t dns_revision_{};
    // Sealed private key of the certificate the order is finalized with.
    std::string private_key_envelope_;
};

RUVIA_MODEL(issuance_authorization_dto,
    RUVIA_REQUIRED_FIELD_NAME("authorization_url", authorization_url, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("challenge_url", challenge_url, ruvia::string));
RUVIA_MODEL(issuance_state_dto, RUVIA_REQUIRED_FIELD(revision, ruvia::int64), RUVIA_REQUIRED_FIELD(phase, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("account_url", account_url, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("order_url", order_url, ruvia::string),
    RUVIA_REQUIRED_FIELD(authorizations, ruvia::array<issuance_authorization_dto>),
    RUVIA_OPTIONAL_FIELD_NAME("dns_revision", dns_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", private_key_envelope, ruvia::string));

[[nodiscard]] inline constexpr std::string_view issuance_phase_name(issuance_phase phase) noexcept {
    return phase == issuance_phase::finalize ? "finalize" : "validation";
}

[[nodiscard]] inline std::string serialize_issuance_state(const issuance_state& state,
    std::pmr::memory_resource* resource) {
    issuance_state_dto output({.resource_ = resource});
    output.set<"revision">(state.revision_);
    output.set<"phase">(issuance_phase_name(state.phase_));
    output.set<"account_url">(state.account_url_);
    output.set<"order_url">(state.order_url_);
    if (state.dns_revision_ > 0) {
        output.set<"dns_revision">(state.dns_revision_);
    }
    auto& authorizations = output.ensure<"authorizations">();
    for (const auto& authorization : state.authorizations_) {
        auto& item = authorizations.emplace_back(ruvia::model_options{.resource_ = resource});
        item.set<"authorization_url">(authorization.authorization_url_);
        item.set<"challenge_url">(authorization.challenge_url_);
    }
    output.set<"private_key_envelope">(state.private_key_envelope_);
    const auto json = ruvia::to_json(output, {.resource_ = resource});
    return std::string(json.data(), json.size());
}

// The stored progress of issuance revision `revision`; nullopt when there is
// none, it belongs to another revision, or it cannot be read (the attempt
// then starts a new order).
[[nodiscard]] inline std::optional<issuance_state> parse_issuance_state(std::string_view json,
    std::int64_t revision, std::pmr::memory_resource* resource) {
    if (json.empty()) {
        return std::nullopt;
    }
    const std::optional<issuance_state_dto> parsed = ruvia::from_json<issuance_state_dto>(json, {.resource_ = resource});
    if (!parsed || parsed->get<"revision">().value_ != revision) {
        return std::nullopt;
    }
    const auto phase = parsed->get<"phase">().view();
    if (phase != "validation" && phase != "finalize") {
        return std::nullopt;
    }
    issuance_state state{.revision_ = revision,
        .phase_ = phase == "finalize" ? issuance_phase::finalize : issuance_phase::validation,
        .account_url_ = std::string(parsed->get<"account_url">().view()),
        .order_url_ = std::string(parsed->get<"order_url">().view()),
        .authorizations_ = {},
        .dns_revision_ = parsed->get<"dns_revision">() ? parsed->get<"dns_revision">()->value_ : 0,
        .private_key_envelope_ = std::string(parsed->get<"private_key_envelope">().view())};
    for (const auto& authorization : parsed->get<"authorizations">()) {
        state.authorizations_.push_back({.authorization_url_ = std::string(authorization.get<"authorization_url">().view()),
            .challenge_url_ = std::string(authorization.get<"challenge_url">().view())});
    }
    // Older validation progress did not record the required DNS revision.
    // Rebuild that order and its challenges instead of assuming publication.
    if (state.order_url_.empty() || (state.phase_ == issuance_phase::validation && state.dns_revision_ <= 0) ||
        (state.phase_ == issuance_phase::finalize && state.private_key_envelope_.empty())) {
        return std::nullopt;
    }
    return state;
}

}  // namespace service::certificate_issuance
