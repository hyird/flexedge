#pragma once

#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace service::certificate_issuance {

// An authorization of the order that still needs its DNS-01 challenge.
struct issuance_authorization final {
    std::string authorization_url_;
    std::string challenge_url_;
};

struct order_identity final {
    std::int64_t revision_{};
    std::string account_url_;
    std::string order_url_;
};

// Transient only: no DNS revision exists until the challenge publication
// transaction returns the real zone revision. This type cannot be persisted.
struct unpublished_order final {
    order_identity order_;
    std::vector<issuance_authorization> authorizations_;
};

class validation_progress final {
public:
    validation_progress(order_identity order, std::vector<issuance_authorization> authorizations,
        std::int64_t dns_revision)
        : order_(std::move(order)),
          authorizations_(std::move(authorizations)),
          dns_revision_(dns_revision) {
        if (order_.order_url_.empty() || dns_revision_ <= 0) {
            throw std::invalid_argument("DNS validation progress requires a published order revision");
        }
    }

    [[nodiscard]] const order_identity& order() const noexcept {
        return order_;
    }
    [[nodiscard]] const std::vector<issuance_authorization>& authorizations() const noexcept {
        return authorizations_;
    }
    [[nodiscard]] std::int64_t dns_revision() const noexcept {
        return dns_revision_;
    }

private:
    friend class finalize_progress;
    order_identity order_;
    std::vector<issuance_authorization> authorizations_;
    std::int64_t dns_revision_;
};

class finalize_progress final {
public:
    finalize_progress(order_identity order, std::string private_key_envelope, std::int64_t dns_revision = 0)
        : order_(std::move(order)),
          private_key_envelope_(std::move(private_key_envelope)),
          dns_revision_(dns_revision) {
        if (order_.order_url_.empty() || private_key_envelope_.empty()) {
            throw std::invalid_argument("Finalize progress requires an order and its certificate key");
        }
    }

    finalize_progress(validation_progress&& validated, std::string private_key_envelope)
        : finalize_progress(std::move(validated.order_), std::move(private_key_envelope), validated.dns_revision_) {}

    [[nodiscard]] const order_identity& order() const noexcept {
        return order_;
    }
    [[nodiscard]] const std::string& private_key_envelope() const noexcept {
        return private_key_envelope_;
    }
    [[nodiscard]] std::int64_t dns_revision() const noexcept {
        return dns_revision_;
    }

private:
    order_identity order_;
    std::string private_key_envelope_;
    std::int64_t dns_revision_;
};

// Only these two resumable stages can reach sys_certificate.issuance_state.
using issuance_progress = std::variant<validation_progress, finalize_progress>;

[[nodiscard]] inline const order_identity& progress_order(const issuance_progress& progress) {
    return std::visit([](const auto& stage) -> const order_identity& { return stage.order(); }, progress);
}

[[nodiscard]] inline std::string_view progress_phase_name(const issuance_progress& progress) noexcept {
    return std::holds_alternative<validation_progress>(progress) ? "validation" : "finalize";
}

RUVIA_MODEL(issuance_authorization_dto,
    RUVIA_REQUIRED_FIELD_NAME("authorization_url", authorization_url, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("challenge_url", challenge_url, ruvia::string));
RUVIA_MODEL(issuance_state_dto, RUVIA_REQUIRED_FIELD(revision, ruvia::int64), RUVIA_REQUIRED_FIELD(phase, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("account_url", account_url, ruvia::string),
    RUVIA_REQUIRED_FIELD_NAME("order_url", order_url, ruvia::string),
    RUVIA_REQUIRED_FIELD(authorizations, ruvia::array<issuance_authorization_dto>),
    RUVIA_OPTIONAL_FIELD_NAME("dns_revision", dns_revision, ruvia::int64),
    RUVIA_REQUIRED_FIELD_NAME("private_key_envelope", private_key_envelope, ruvia::string));

template <typename progress_type>
    requires(std::is_same_v<progress_type, validation_progress> || std::is_same_v<progress_type, finalize_progress>)
[[nodiscard]] inline std::string serialize_issuance_progress(const progress_type& progress,
    std::pmr::memory_resource* resource) {
    issuance_state_dto output({.resource_ = resource});
    const auto& order = progress.order();
    output.set<"revision">(order.revision_);
    output.set<"phase">(std::is_same_v<progress_type, validation_progress> ? "validation" : "finalize");
    output.set<"account_url">(order.account_url_);
    output.set<"order_url">(order.order_url_);
    auto& authorizations = output.ensure<"authorizations">();
    if (progress.dns_revision() > 0) {
        output.set<"dns_revision">(progress.dns_revision());
    }
    if constexpr (std::is_same_v<progress_type, validation_progress>) {
        for (const auto& authorization : progress.authorizations()) {
            auto& item = authorizations.emplace_back(ruvia::model_options{.resource_ = resource});
            item.set<"authorization_url">(authorization.authorization_url_);
            item.set<"challenge_url">(authorization.challenge_url_);
        }
        output.set<"private_key_envelope">("");
    } else {
        output.set<"private_key_envelope">(progress.private_key_envelope());
    }
    const auto json = ruvia::to_json(output, {.resource_ = resource});
    return std::string(json.data(), json.size());
}

[[nodiscard]] inline std::string serialize_issuance_progress(const issuance_progress& progress,
    std::pmr::memory_resource* resource) {
    return std::visit([&](const auto& stage) { return serialize_issuance_progress(stage, resource); }, progress);
}

// The stored progress of issuance revision `revision`; nullopt when there is
// none, it belongs to another revision, or it cannot be read (the attempt
// then starts a new order).
[[nodiscard]] inline std::optional<issuance_progress> parse_issuance_progress(std::string_view json,
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
    order_identity order{.revision_ = revision,
        .account_url_ = std::string(parsed->get<"account_url">().view()),
        .order_url_ = std::string(parsed->get<"order_url">().view())};
    if (order.order_url_.empty()) {
        return std::nullopt;
    }
    const auto dns_revision = parsed->get<"dns_revision">() ? parsed->get<"dns_revision">()->value_ : 0;
    if (phase == "finalize") {
        const auto envelope = parsed->get<"private_key_envelope">().view();
        if (envelope.empty()) {
            return std::nullopt;
        }
        return finalize_progress(std::move(order), std::string(envelope), dns_revision);
    }
    // Older validation progress did not record the required DNS revision.
    // Rebuild that order and its challenges instead of assuming publication.
    if (dns_revision <= 0) {
        return std::nullopt;
    }
    std::vector<issuance_authorization> authorizations;
    for (const auto& authorization : parsed->get<"authorizations">()) {
        authorizations.push_back({.authorization_url_ = std::string(authorization.get<"authorization_url">().view()),
            .challenge_url_ = std::string(authorization.get<"challenge_url">().view())});
    }
    return validation_progress(std::move(order), std::move(authorizations), dns_revision);
}

}  // namespace service::certificate_issuance
