#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/sha256.h"
#include "node/data/cc_protection.h"
#include "node/data/request_expression.h"
#include "node/runtime/compiled_config.h"

namespace flexedge::node {

struct security_decision final {
    std::uint16_t status_{};
    std::string_view content_type_{}, body_{};
    std::string rule_{};
    bool skip_cc_{}, skip_managed_{};
    std::vector<std::string> logged_rules_{};
};

// Owned by one worker_execution_view. RE2 and list indexes never cross Workers.
class security_program final {
public:
    explicit security_program(const compiled_config& config) {
        for (const auto* website : config.websites()) {
            if (!website->has_security()) {
                continue;
            }
            const auto& source = website->security();
            const auto identity = source.id() + ":" + std::to_string(source.revision());
            auto found = policies_.find(identity);
            if (found == policies_.end()) {
                policy value;
                auto counter_source = source;
                for (auto& list : *counter_source.mutable_lists()) {
                    list.clear_entries();
                    list.clear_members();
                }
                value.fingerprint_ = artifact_digest(serialize_artifact(counter_source));
                for (const auto& list : source.lists()) {
                    std::vector<request_list_value> entries;
                    for (const auto& item : list.entries()) {
                        entries.push_back({item});
                    }
                    for (const auto& item : list.members()) {
                        entries.push_back({item.value(), item.expires_unix_ms()});
                    }
                    if (list.entries_size() > 100000 || list.name().empty() || list.name().size() > 100 ||
                        !value.lists_.emplace(list.name(), std::make_shared<const request_match_list>(list.type(),
                                                               entries))
                            .second) {
                        throw std::invalid_argument("invalid security list");
                    }
                }
                for (const auto& rule : source.custom_rules()) {
                    value.custom_.push_back({&rule, request_expression_program(rule.expression(), flexedge::rules::expression_context::security, value.lists_)});
                }
                for (const auto& rule : source.cc_rules()) {
                    value.cc_.push_back({&rule, request_expression_program(rule.expression(), flexedge::rules::expression_context::security, value.lists_)});
                }
                found = policies_.emplace(identity, std::move(value)).first;
            }
            websites_.emplace(website->id(), &found->second);
        }
    }

    security_decision evaluate(const v2::Website& website, const rule_match_request& request,
        cc_worker& counters, std::uint64_t now) const {
        const auto found = websites_.find(website.id());
        if (found == websites_.end()) {
            return {};
        }
        const auto& policy = *found->second;
        security_decision decision;
        for (const auto& rule : policy.custom_) {
            if (!rule.source_->enabled() || !rule.expression_.matches(request)) {
                continue;
            }
            const auto& source = *rule.source_;
            if (source.action() == "skip") {
                decision.rule_ = source.id();
                bool skip_custom = source.skip_stages().empty();
                decision.skip_cc_ = decision.skip_cc_ || source.skip_stages().empty();
                decision.skip_managed_ = decision.skip_managed_ || source.skip_stages().empty();
                for (const auto& stage : source.skip_stages()) {
                    skip_custom = skip_custom || stage == "custom";
                    decision.skip_cc_ = decision.skip_cc_ || stage == "rate";
                    decision.skip_managed_ = decision.skip_managed_ || stage == "managed";
                }
                decision.logged_rules_.push_back(source.id());
                if (skip_custom) {
                    break;
                }
                continue;
            }
            if (source.action() == "block") {
                decision.status_ = source.response_status() ? static_cast<std::uint16_t>(source.response_status()) : std::uint16_t{403};
                decision.content_type_ = source.response_content_type();
                decision.body_ = source.response_body();
                decision.rule_ = source.id();
                return decision;
            }
            decision.logged_rules_.push_back(source.id());
        }
        for (const auto& rule : policy.cc_) {
            if (decision.skip_cc_) {
                break;
            }
            if (!rule.source_->enabled() || !rule.expression_.matches(request)) {
                continue;
            }
            const auto& source = *rule.source_;
            const auto outcome = counters.check({website.id(), policy.fingerprint_, source.id(), std::string(request.client_ip_)},
                {source.requests(), source.window_seconds() * 1000, source.mitigation_seconds() * 1000}, now);
            if (outcome == cc_decision::overloaded && source.action() == "block") {
                decision.status_ = 503;
                decision.rule_ = source.id();
                return decision;
            }
            if (outcome == cc_decision::block && source.action() == "block") {
                decision.status_ = source.response_status() ? static_cast<std::uint16_t>(source.response_status()) : std::uint16_t{429};
                decision.content_type_ = source.response_content_type();
                decision.body_ = source.response_body();
                decision.rule_ = source.id();
                return decision;
            }
            if (outcome != cc_decision::allow) {
                decision.logged_rules_.push_back(source.id());
            }
        }
        return decision;
    }

private:
    struct custom_rule final {
        const v2::SecurityRule* source_{};
        request_expression_program expression_;
    };
    struct cc_rule final {
        const v2::CcRule* source_{};
        request_expression_program expression_;
    };
    struct policy final {
        std::string fingerprint_{};
        request_expression_program::list_map lists_{};
        std::vector<custom_rule> custom_{};
        std::vector<cc_rule> cc_{};
    };
    std::unordered_map<std::string, policy> policies_;
    std::unordered_map<std::string, const policy*> websites_;
};

}  // namespace flexedge::node
