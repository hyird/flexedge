#include <iostream>

#include "node/proto/edge_control.pb.h"
#include "service/features/cache_config/defaults.h"
#include "service/features/cache_config/mapper.h"

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}
void verify() {
    auto policy = service::cache_config::default_policy();
    require(policy.rules_.size() == 1 && policy.rules_[0].action_ == "cache" && policy.rules_[0].patterns_.size() == 97 &&
                policy.rules_[0].settings_->armor_ && policy.rules_[0].browser_ttl_mode_ == "respect_origin",
        "default preset is a broad opt-in static allowlist with origin safeguards");
    require(!policy.rules_.front().skip_request_cookie_ && !policy.rules_.front().respect_request_no_cache_ &&
                policy.rules_.front().ttl_mode_ == "origin" && policy.rules_.front().browser_ttl_mode_ == "respect_origin",
        "static defaults use fresh cache for cookies and refreshes");
    for (const bool enabled : {false, true, false}) {
        auto& rule = policy.rules_.front();
        rule.skip_request_cookie_ = enabled;
        rule.respect_request_no_cache_ = enabled;
        rule.skip_no_cache_ = enabled;
        rule.browser_ttl_mode_ = enabled ? "override_origin" : "respect_origin";
        rule.browser_ttl_seconds_ = 123;
        const auto json = ruvia::to_json(service::cache_config::to_dto(policy));
        const auto loaded = service::cache_config::parse(json);
        const auto& saved = loaded.rules_.front();
        require(saved.skip_request_cookie_ == enabled && saved.respect_request_no_cache_ == enabled &&
                    saved.skip_no_cache_ == enabled && saved.browser_ttl_mode_ == rule.browser_ttl_mode_ && saved.browser_ttl_seconds_ == 123,
            "advanced booleans and browser TTL survive JSON persistence");
        flexedge::node::v2::CachePolicy wire;
        flexedge::cache::to_proto(loaded, wire);
        const auto decoded = flexedge::cache::from_proto(wire);
        require(ruvia::to_json(service::cache_config::to_dto(decoded)) == json,
            "persisted advanced settings survive node protocol round trip");
    }
    auto legacy = service::cache_config::to_dto(policy);
    auto extended = policy;
    auto& settings = extended.rules_.front().settings_.emplace();
    settings.enabled_ = {"edge", "browser", "status", "key", "armor", "device", "sort", "stale", "etag", "origin", "vary", "range", "ports", "timeout", "errors"};
    settings.status_ttl_ = {{200, 299, 3600}, {500, 599, -1}};
    settings.key_.query_mode_ = "include";
    settings.key_.query_parameters_ = {"v", "lang"};
    settings.key_.headers_ = {"x-variant"};
    settings.key_.header_presence_ = {"x-preview"};
    settings.key_.header_values_ = {{"accept", {"application/json"}}};
    settings.key_.cookies_ = {"currency"};
    settings.key_.cookie_presence_ = {"preview"};
    settings.key_.country_ = true;
    settings.key_.language_ = true;
    settings.serve_stale_ = false;
    settings.origin_control_ = false;
    settings.vary_headers_ = {{"accept-language", "normalize", {"en", "zh"}}};
    settings.origin_range_ = "on";
    settings.ports_ = {8443, 8080};
    settings.read_timeout_ = 300;
    const auto extended_json = ruvia::to_json(service::cache_config::to_dto(extended));
    const auto persisted = service::cache_config::parse(extended_json);
    flexedge::node::v2::CachePolicy extended_wire;
    flexedge::cache::to_proto(persisted, extended_wire);
    require(extended_json == ruvia::to_json(service::cache_config::to_dto(flexedge::cache::from_proto(extended_wire))), "all optional settings survive persistence and node delivery");
    require(!persisted.rules_.front().settings_->serve_stale_ && !persisted.rules_.front().settings_->origin_control_, "explicit false settings survive round trip");
    auto& legacy_rule = legacy.ensure<"rules">().front();
    legacy_rule.reset<"skip_request_cookie">();
    legacy_rule.reset<"respect_request_no_cache">();
    legacy_rule.reset<"skip_no_cache">();
    legacy_rule.reset<"browser_ttl_mode">();
    const auto decoded = service::cache_config::parse(ruvia::to_json(legacy));
    const auto& rule = decoded.rules_.front();
    require(!rule.skip_request_cookie_ && !rule.respect_request_no_cache_ && rule.skip_no_cache_,
        "missing optional fields follow CF refresh and cookie defaults");
}
}  // namespace

int main() {
    try {
        verify();
        std::cout << "cache configuration passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
