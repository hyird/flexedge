#include "node/data/response_cache.h"

#include <barrier>
#include <iostream>
#include <set>
#include <thread>

#include "node/runtime/country_database.h"
#include "tests/support/cache_directory.h"

#define FLEXEDGE_REQUIRE(value)                                   \
    do {                                                          \
        if (!(value)) {                                           \
            throw std::runtime_error("cache invariant: " #value); \
        }                                                         \
    } while (false)
using namespace flexedge::node;
namespace {
v2::Website website() {
    v2::Website site;
    site.set_id("site");
    site.set_revision(1);
    flexedge::cache::policy policy;
    policy.storage_ = "memory";
    policy.id_ = "policy";
    flexedge::cache::rule rule;
    rule.id_ = "all";
    rule.name_ = "all";
    rule.match_type_ = "all";
    rule.skip_request_cookie_ = true;
    rule.ttl_mode_ = "fixed";
    rule.ttl_seconds_ = 600;
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    return site;
}
struct response_value final {
    std::uint16_t status_{200};
    cache_headers headers_{};
    std::string body_{};
};
bool store(response_cache& cache, const cache_ticket& ticket, const v2::CachePolicy& policy,
    const flexedge::cache::rule& rule, const response_value& value, const cache_headers& request) {
    auto fill = cache.begin_fill(policy, rule, {value.status_, value.headers_}, {}, cache_has_header(request, "Authorization"));
    if (!fill || !fill->append(value.body_)) {
        cache.abandon(ticket);
        return false;
    }
    return cache.store(ticket, policy, rule, *fill, request);
}
response_value response(std::string body = "body") {
    response_value value;
    value.status_ = 200;
    value.body_ = std::move(body);
    value.headers_ = {{"Content-Type", "text/plain"}, {"Cache-Control", "public, max-age=600"}};
    return value;
}
void directive_boundaries() {
    FLEXEDGE_REQUIRE(!cache_has_directive(R"(public, extension="a,no-store,b")", "no-store"));
    FLEXEDGE_REQUIRE(!cache_has_directive(R"(extension="a,private,b", public)", "private"));
    FLEXEDGE_REQUIRE(!cache_has_directive(R"(extension="a\",no-cache,b", public)", "no-cache"));
    FLEXEDGE_REQUIRE(cache_has_directive(R"(extension="a\\", No-StOrE)", "no-store"));
    FLEXEDGE_REQUIRE(cache_has_directive(R"(extension="a,no-store,b", no-store)", "no-store"));
    FLEXEDGE_REQUIRE(cache_directive_seconds(R"(extension="a,max-age=0,b", MaX-AgE="600")", "max-age") == 600);
    FLEXEDGE_REQUIRE(cache_directive_seconds(R"(max-age="6\0")", "max-age") == 60);
    FLEXEDGE_REQUIRE(cache_directive_seconds(" , max-age=60, , ", "max-age") == 60);
    FLEXEDGE_REQUIRE(!cache_directive_seconds(R"(extension="max-age=60")", "max-age"));
    const cache_headers combined{{"Cache-Control", "max-age=600"}, {"cache-control", "max-age=10"}};
    FLEXEDGE_REQUIRE(cache_directive_seconds(cache_header(combined, "Cache-Control"), "max-age") == 0);
    for (const auto value : {"max-age=", "max-age=-1", "max-age=60s", "max-age=\"60",
             "max-age=9999999999999999999999999999999999999x"}) {
        FLEXEDGE_REQUIRE(cache_directive_seconds(value, "max-age") == 0);
    }
    FLEXEDGE_REQUIRE(cache_directive_seconds("max-age=9999999999999999999999999999999999999", "max-age") == 31536000);
}

void cloudflare_defaults() {
    flexedge::testing::temporary_cache_directory directory;
    auto publication = std::make_shared<response_cache_publication>(directory.path_);
    response_cache cache(publication);
    flexedge::cache::policy policy;
    policy.id_ = "cf-default";
    policy.storage_ = "memory";
    policy.max_object_bytes_ = policy.memory_bytes_;
    v2::Website site;
    site.set_id("cf-site");
    site.set_revision(1);
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    FLEXEDGE_REQUIRE(!cache.matching(site, "/app.js", "GET"));
    FLEXEDGE_REQUIRE(!cache.matching(site, "/robots.txt", "GET"));
    FLEXEDGE_REQUIRE(!cache.matching(site, "/index.html", "GET"));
    FLEXEDGE_REQUIRE(!cache.matching(site, "/api.json", "GET"));
    auto rule = flexedge::cache::default_static_rule();
    FLEXEDGE_REQUIRE(!rule.skip_request_cookie_ && !rule.respect_request_no_cache_);
    FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {{"Cache-Control", "public, max-age=600"}}}) == 14400);
    FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {{"Cache-Control", "public, max-age=31536000, immutable"}}}) == 0);
    for (const auto control : {"private", "no-store", "no-cache"}) {
        FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {{"Cache-Control", control}}}) == 0);
    }
    rule.browser_ttl_mode_ = "respect_origin";
    FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {}}) == 0);
    rule.browser_ttl_mode_ = "override_origin";
    rule.browser_ttl_seconds_ = 120;
    FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {{"Cache-Control", "public, max-age=600"}}}) == 120);
    rule.browser_ttl_mode_ = "bypass";
    FLEXEDGE_REQUIRE(cache_browser_ttl(rule, {200, {}}) == -1);
    rule.browser_ttl_mode_ = "default";
    rule.browser_ttl_seconds_ = 0;
    for (const auto [status, seconds] : std::array<std::pair<std::uint16_t, int>, 6>{{{200, 7200}, {301, 7200}, {302, 1200}, {303, 1200}, {404, 180}, {410, 180}}}) {
        const auto url = "http://example.com/status/" + std::to_string(status);
        const auto ticket = cache.lookup(site, rule, url, {});
        FLEXEDGE_REQUIRE(store(cache, *ticket.ticket_, site.response_cache(), rule, {status, {}, "body"}, {}));
        const auto hit = cache.lookup(site, rule, url, {}, false).response_;
        FLEXEDGE_REQUIRE(hit && std::chrono::duration_cast<std::chrono::seconds>(hit->expires_at_ - hit->stored_at_).count() == seconds);
    }
    const auto warm = cache.lookup(site, rule, "http://example.com/cors.js", {{"Origin", "https://first.example"}});
    FLEXEDGE_REQUIRE(store(cache, *warm.ticket_, site.response_cache(), rule, response(), {{"Origin", "https://first.example"}}));
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://example.com/cors.js", {{"Origin", "https://second.example"}}, false).response_);
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://alias.example.com/cors.js", {{"Origin", "https://first.example"}}, false).response_);
    FLEXEDGE_REQUIRE(cache.lookup(site, rule, "http://example.com/cors.js", {{"Origin", "https://first.example"}}, false).response_);
    const auto auth = cache.lookup(site, rule, "http://example.com/auth.js", {});
    auto private_value = response();
    private_value.headers_ = {{"Cache-Control", "max-age=600"}};
    FLEXEDGE_REQUIRE(store(cache, *auth.ticket_, site.response_cache(), rule, private_value, {}));
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://example.com/auth.js", {{"Authorization", "Bearer token"}}, false).response_);
    const auto authenticated = cache.lookup(site, rule, "http://example.com/auth.js", {{"Authorization", "Bearer token"}});
    FLEXEDGE_REQUIRE(authenticated.ticket_ && !store(cache, *authenticated.ticket_, site.response_cache(), rule, private_value, {{"Authorization", "Bearer token"}}));
    auto strict = rule;
    strict.ttl_mode_ = "origin_strict";
    const auto missing_ttl = cache.lookup(site, strict, "http://example.com/strict.js", {});
    FLEXEDGE_REQUIRE(!store(cache, *missing_ttl.ticket_, site.response_cache(), strict, {200, {}, "body"}, {}));
    const cache_headers cdn_headers{{"Cache-Control", "private, max-age=10"},
        {"CDN-Cache-Control", "max-age=18000"}, {"Cloudflare-CDN-Cache-Control", "max-age=24400"}};
    const auto cdn = cache.lookup(site, rule, "http://example.com/cdn.js", {});
    FLEXEDGE_REQUIRE(store(cache, *cdn.ticket_, site.response_cache(), rule, {200, cdn_headers, "body"}, {}));
    const auto cdn_hit = cache.lookup(site, rule, "http://example.com/cdn.js", {}, false).response_;
    FLEXEDGE_REQUIRE(cdn_hit && std::chrono::duration_cast<std::chrono::seconds>(cdn_hit->expires_at_ - cdn_hit->stored_at_).count() == 24400);
    FLEXEDGE_REQUIRE(cdn_hit->browser_ttl_seconds_ == 0);
    FLEXEDGE_REQUIRE(cache_edge_control({{"Cache-Control", "private"}, {"CDN-Cache-Control", "public"}}) == "public");
    FLEXEDGE_REQUIRE(cache_edge_control({{"Cache-Control", "private"}, {"Surrogate-Control", ""}}).empty());
    FLEXEDGE_REQUIRE(cache_allows_authorization({{"CDN-Cache-Control", "s-maxage=0"}}));
    for (const bool present : {false, true}) {
        auto qualified = cache_headers{{"Cache-Control", "public, max-age=600, no-cache=\"X-Validate\", private=\"X-Secret\""}, {"X-Secret", "hidden"}};
        if (present) {
            qualified.emplace_back("X-Validate", "validate");
        }
        const auto url = std::string("http://example.com/qualified-") + (present ? "present.js" : "missing.js");
        const auto ticket = cache.lookup(site, rule, url, {});
        FLEXEDGE_REQUIRE(store(cache, *ticket.ticket_, site.response_cache(), rule, {200, qualified, "body"}, {}));
        const auto lookup = cache.lookup(site, rule, url, {}, false);
        const auto stored = present ? lookup.stale_ : lookup.response_;
        FLEXEDGE_REQUIRE(stored && !cache_has_header(stored->headers_, "X-Secret"));
        FLEXEDGE_REQUIRE((stored->expires_at_ <= stored->stored_at_) == present);
    }
    policy.rules_ = {{.id_ = "early", .name_ = "early", .match_type_ = "all"},
        {.id_ = "late", .name_ = "late", .match_type_ = "exact", .patterns_ = {"/app.js"}, .action_ = "bypass"}};
    ++policy.revision_;
    site.clear_response_cache();
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    const auto selected = cache.matching(site, "/app.js", "GET");
    FLEXEDGE_REQUIRE(selected && selected->id_ == "late" && selected->action_ == "bypass");
}

void optional_settings() {
    flexedge::testing::temporary_cache_directory directory;
    auto publication = std::make_shared<response_cache_publication>(directory.path_);
    response_cache cache(publication);
    auto site = website();
    auto rule = flexedge::cache::from_proto(site.response_cache()).rules_.front();
    rule.ttl_mode_ = "origin";
    rule.skip_request_cookie_ = false;
    auto& settings = rule.settings_.emplace();
    settings.enabled_ = {"key", "sort", "status", "vary"};
    settings.key_.query_mode_ = "include";
    settings.key_.query_parameters_ = {"v", "lang"};
    settings.key_.cookies_ = {"currency"};
    settings.sort_query_ = true;
    settings.vary_default_ = "bypass";
    settings.vary_headers_ = {{"accept-language", "normalize", {"en", "zh"}}};
    settings.status_ttl_ = {{200, 299, 3600}, {404, 404, 0}, {500, 599, -1}};
    FLEXEDGE_REQUIRE(flexedge::cache::validate_settings(settings).empty());
    const auto country_rule = [&] { auto value = rule; value.settings_->key_.country_ = true; value.settings_->key_.host_ = "resolved"; return value; }();
    const auto partition = [&](std::string_view host, std::string_view country) {
        return cache.lookup(site, country_rule, "https://example.com/geo.js", {}, false, false, host, country).key_;
    };
    FLEXEDGE_REQUIRE(partition("origin.example", "CN") != partition("origin.example", "US"));
    FLEXEDGE_REQUIRE(partition("origin.example", "CN") != partition("another.example", "CN"));
    const auto before_host_header = partition("origin.example", "CN");
    site.set_origin_host_header("host-header.example");
    FLEXEDGE_REQUIRE(partition("origin.example", "CN") == before_host_header);
    v2::CountryDatabase countries;
    const auto add_country = [](auto* ranges, std::string_view first, std::string_view last, std::string_view country) {
        auto* range = ranges->Add();
        const auto start = asio::ip::make_address(first);
        const auto end = asio::ip::make_address(last);
        const auto bytes = [&](const auto& left, const auto& right) {
            range->set_first(left.data(), left.size());
            range->set_last(right.data(), right.size());
            range->set_country(country);
        };
        if (start.is_v4()) {
            bytes(start.to_v4().to_bytes(), end.to_v4().to_bytes());
        } else {
            bytes(start.to_v6().to_bytes(), end.to_v6().to_bytes());
        }
    };
    add_country(countries.mutable_ipv4(), "1.0.0.0", "1.0.0.255", "AU");
    add_country(countries.mutable_ipv4(), "8.8.8.0", "8.8.8.255", "US");
    add_country(countries.mutable_ipv6(), "2001:db8::", "2001:db8::ffff", "ZZ");
    validate_country_database(countries);
    FLEXEDGE_REQUIRE(country_for_address(countries, "1.0.0.255") == "AU");
    FLEXEDGE_REQUIRE(country_for_address(countries, "1.0.1.0") == "XX");
    FLEXEDGE_REQUIRE(country_for_address(countries, "8.8.8.8") == "US");
    FLEXEDGE_REQUIRE(country_for_address(countries, "2001:db8::1234") == "ZZ");
    FLEXEDGE_REQUIRE(country_for_address(countries, "not-an-address") == "XX");
    auto invalid_country = countries;
    invalid_country.mutable_ipv4(1)->set_first(countries.ipv4(0).first());
    bool rejected_country = false;
    try {
        validate_country_database(invalid_country);
    } catch (const std::runtime_error&) {
        rejected_country = true;
    }
    FLEXEDGE_REQUIRE(rejected_country);
    const cache_headers request{{"Cookie", "currency=USD; sid=first"}, {"Accept-Language", "en-US, zh;q=0.8"}};
    auto value = response();
    value.headers_ = {{"Cache-Control", "private, no-store"}, {"Vary", "Accept-Language"}, {"Set-Cookie", "sid=first"}};
    auto ticket = cache.lookup(site, rule, "http://example.com/app.js?v=1&tracking=a&lang=en", request);
    FLEXEDGE_REQUIRE(store(cache, *ticket.ticket_, site.response_cache(), rule, value, request));
    const auto hit = cache.lookup(site, rule, "http://example.com/app.js?lang=en&v=1&tracking=b", {{"Cookie", "sid=second; currency=USD"}, {"Accept-Language", "zh;q=0.8, en-GB"}}, false).response_;
    FLEXEDGE_REQUIRE(hit && !cache_has_header(hit->headers_, "Set-Cookie"));
    FLEXEDGE_REQUIRE(std::chrono::duration_cast<std::chrono::seconds>(hit->expires_at_ - hit->stored_at_).count() == 3600);
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://example.com/app.js?lang=en&v=2", request, false).response_);
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://example.com/app.js?lang=en&v=1", {{"Cookie", "currency=EUR"}, {"Accept-Language", "en-US, zh;q=0.8"}}, false).response_);
    FLEXEDGE_REQUIRE(!cache.lookup(site, rule, "http://example.com/app.js?lang=en&v=1", {{"Cookie", "currency=USD"}, {"Accept-Language", "zh, en;q=0.8"}}, false).response_);
    FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(rule, {503, {}}) == "origin_status_ttl");
    auto missing = cache.lookup(site, rule, "http://example.com/missing.js", request);
    FLEXEDGE_REQUIRE(store(cache, *missing.ticket_, site.response_cache(), rule, {404, {}, "missing"}, request));
    const auto expired = cache.lookup(site, rule, "http://example.com/missing.js", request, false);
    FLEXEDGE_REQUIRE(expired.stale_ && !expired.response_ && !response_cache::usable_stale(*expired.stale_));
    auto no_header = response();
    no_header.headers_.emplace_back("Vary", "User-Agent");
    FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(rule, {200, no_header.headers_}) == "origin_vary");
    FLEXEDGE_REQUIRE(normalize_cache_vary("fr;q=0.8, en-GB", "accept-language") == "en,fr");
    FLEXEDGE_REQUIRE(normalize_cache_vary("application/json;q=0, text/html", "accept") == "text/html,application/json;q=0");
    FLEXEDGE_REQUIRE(normalize_cache_vary("en-US, fr;q=0.8", "accept-language", {"en-us", "fr"}) == "en-us,fr");
    auto generic = rule;
    generic.settings_->vary_default_ = "normalize";
    FLEXEDGE_REQUIRE(cache_request_vary(generic, {{"X-Custom", " \"Value2, Value1\" "}, {"X-Custom", " Value3 "}}, "x-custom", "identity") == "\"Value2, Value1\",Value3");
    auto strong_rule = rule;
    strong_rule.settings_->enabled_.push_back("etag");
    strong_rule.settings_->strong_etags_ = true;
    FLEXEDGE_REQUIRE(cache.lookup(site, strong_rule, "http://example.com/etag.js", {{"Accept-Encoding", "gzip"}}, false).key_ != cache.lookup(site, strong_rule, "http://example.com/etag.js", {{"Accept-Encoding", "br"}}, false).key_);
    auto small_site = site;
    small_site.mutable_response_cache()->set_max_object_bytes(origin_range_block_bytes);
    auto partial_claim = cache.lookup(small_site, rule, "http://example.com/large-range", {}, true, false, {}, "XX", "range:0");
    auto partial_body = response("a");
    partial_body.status_ = 206;
    partial_body.headers_ = {{"Content-Range", "bytes 0-0/2097152"}};
    FLEXEDGE_REQUIRE(!store(cache, *partial_claim.ticket_, small_site.response_cache(), rule, partial_body, {}));
    auto later = rule;
    later.id_ = "browser-only";
    later.settings_->enabled_ = {"browser"};
    later.browser_ttl_mode_ = "respect_origin";
    later.ttl_seconds_ = 99;
    rule.ttl_mode_ = "fixed";
    rule.ttl_seconds_ = 600;
    flexedge::cache::apply_rule(rule, later);
    FLEXEDGE_REQUIRE(rule.ttl_seconds_ == 600 && rule.browser_ttl_mode_ == "respect_origin" && flexedge::cache::configured(rule, "key"));
    auto stale = *hit;
    stale.headers_ = {{"Cache-Control", "max-age=1, stale-while-revalidate=60"}};
    stale.expires_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    FLEXEDGE_REQUIRE(response_cache::updating_stale(rule, stale));
    rule.settings_->enabled_.push_back("stale");
    rule.settings_->serve_stale_ = false;
    FLEXEDGE_REQUIRE(!response_cache::updating_stale(rule, stale));
    FLEXEDGE_REQUIRE(cache_content_type_matches("/image.jpg", "image/webp"));
    FLEXEDGE_REQUIRE(!cache_content_type_matches("/account.jpg", "text/html"));
    FLEXEDGE_REQUIRE(cache_content_type_matches("/download.zip", "application/octet-stream"));
    auto invalid = settings;
    invalid.status_ttl_.push_back({250, 450, 10});
    FLEXEDGE_REQUIRE(!flexedge::cache::validate_settings(invalid).empty());
}
void expressions() {
    const cache_headers headers{{"Host", "Example.com"}, {"Cookie", "sid=one; preview=1"}, {"User-Agent", "Browser"}};
    const cache_match_request request{"/ASSETS/app.js?v=3", "GET", "example.com", true, headers};
    const auto matches = [&](std::string_view expression) { return cache_expression_program(expression).matches(request); };
    FLEXEDGE_REQUIRE(matches("http.request.uri.path.extension in {\"js\" \"css\"}"));
    FLEXEDGE_REQUIRE(matches("http.host eq \"example.com\" and not http.cookie contains \"admin=\""));
    FLEXEDGE_REQUIRE(matches("starts_with(lower(http.request.uri.path), \"/assets/\") and http.request.uri.query eq \"v=3\""));
    FLEXEDGE_REQUIRE(matches("http.request.uri.path wildcard \"/assets/*.JS\""));
    FLEXEDGE_REQUIRE(!matches("http.request.uri.path strict wildcard \"/assets/*.JS\""));
    FLEXEDGE_REQUIRE(matches("http.request.uri.path matches r\"app\\.js\""));
    FLEXEDGE_REQUIRE(matches("http.request.full_uri eq \"https://Example.com/ASSETS/app.js?v=3\""));
    FLEXEDGE_REQUIRE(matches("http.request.headers[\"user-agent\"][0] eq \"Browser\""));
    FLEXEDGE_REQUIRE(matches("false or true and true"));
    FLEXEDGE_REQUIRE(!matches("(false or true) and false"));
    FLEXEDGE_REQUIRE(matches("true xor false"));
    FLEXEDGE_REQUIRE(!matches("true xor true"));
    FLEXEDGE_REQUIRE(!flexedge::cache::validate_cache_expression("http.host eq \"x\" trailing").empty());
    FLEXEDGE_REQUIRE(!flexedge::cache::validate_cache_expression("cf.bot_management.score eq \"1\"").empty());
    FLEXEDGE_REQUIRE(!flexedge::cache::validate_cache_expression("http.host wildcard \"**\"").empty());
    flexedge::testing::temporary_cache_directory directory;
    response_cache cache(std::make_shared<response_cache_publication>(directory.path_));
    auto site = website();
    auto policy = flexedge::cache::from_proto(site.response_cache());
    policy.max_object_bytes_ = 1024 * 1024;
    policy.rules_.front().match_type_ = "expression";
    policy.rules_.front().patterns_ = {"http.host eq \"example.com\" and http.cookie contains \"preview=\""};
    policy.rules_.front().action_ = "bypass";
    FLEXEDGE_REQUIRE(flexedge::cache::validate(policy).empty());
    site.clear_response_cache();
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    FLEXEDGE_REQUIRE(cache.matching(site, request.target_, "GET", headers, true)->action_ == "bypass");
    FLEXEDGE_REQUIRE(!cache.matching(site, request.target_, "GET", {{"Host", "elsewhere.com"}}, true));
    policy.rules_ = flexedge::cache::safe_static_rules();
    ++policy.revision_;
    site.clear_response_cache();
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    for (const auto path : {"/app.js", "/app.mjs", "/engine.wasm", "/font.woff2", "/movie.m4s", "/package.msix"}) {
        const auto* selected = cache.matching(site, path, "GET");
        FLEXEDGE_REQUIRE(selected && selected->action_ == "cache" && selected->browser_ttl_mode_ == "respect_origin" && selected->settings_->armor_);
        FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(*selected, {200, {{"Cache-Control", "private"}}}) == "origin_private");
        FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(*selected, {200, {{"Cache-Control", "no-store"}}}) == "origin_no_store");
        FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(*selected, {200, {{"Set-Cookie", "session=one"}}}) == "origin_set_cookie");
    }
    for (const auto path : {"/", "/api/data.json", "/index.html", "/template.ejs", "/account.php", "/app.js.map", "/robots.txt"}) {
        FLEXEDGE_REQUIRE(!cache.matching(site, path, "GET"));
    }
    const auto* static_rule = cache.matching(site, "/app.mjs", "GET");
    FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(*static_rule, {200, {{"Content-Type", "text/html"}}}, false, "/app.mjs") == "origin_cache_deception");
    policy.rules_.front().enabled_ = false;
    ++policy.revision_;
    site.clear_response_cache();
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    FLEXEDGE_REQUIRE(!cache.matching(site, "/app.js", "GET"));
    policy.rules_.clear();
    ++policy.revision_;
    site.clear_response_cache();
    flexedge::cache::to_proto(policy, *site.mutable_response_cache());
    FLEXEDGE_REQUIRE(!cache.matching(site, "/app.js", "GET"));
}

void verify() {
    const cache_resource_capacity host{1000, 200};
    FLEXEDGE_REQUIRE(cache_capacity_limit(host, 100, 999, 50, 100) == 200);
    FLEXEDGE_REQUIRE(cache_capacity_limit(host, 400, 999, 50, 100) == 500);
    FLEXEDGE_REQUIRE(cache_capacity_limit(host, 0, 999, 50, 300) == 0);
    FLEXEDGE_REQUIRE(cache_capacity_limit(std::nullopt, 0, 100, 50, 0) == 0);
    FLEXEDGE_REQUIRE(cache_capacity_limit(std::nullopt, 0, 100, 0, 0) == 100);
    const auto directory = std::filesystem::current_path() / ("cache-test-" + std::to_string(std::random_device{}()));
    auto publication = std::make_shared<response_cache_publication>(directory);
    response_cache first(publication), second(publication);
    auto site = website();
    const auto rule = flexedge::cache::from_proto(site.response_cache()).rules_.front();
    auto fill = first.lookup(site, rule, "http://example.com/a", {});
    FLEXEDGE_REQUIRE(fill.ticket_);
    FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://example.com/a", {}).filling_);
    auto value = response();
    FLEXEDGE_REQUIRE(store(first, *fill.ticket_, site.response_cache(), rule, value, {}));
    const auto retained = second.lookup(site, rule, "http://example.com/a", {}).response_;
    FLEXEDGE_REQUIRE(retained && *retained->body_ == "body");
    FLEXEDGE_REQUIRE(!second.lookup(site, rule, "http://alias.example.com/a", {}, false).response_);
    FLEXEDGE_REQUIRE(!second.lookup(site, rule, "https://alias.example.com/a", {}, false).response_);
    FLEXEDGE_REQUIRE(!second.lookup(site, rule, "http://alias.example.com/a?q=1", {}, false).response_);
    auto other_site = site;
    other_site.set_id("other-site");
    FLEXEDGE_REQUIRE(!second.lookup(other_site, rule, "http://alias.example.com/a", {}, false).response_);
    auto in_flight = first.lookup(site, rule, "http://example.com/b", {});
    v2::CacheCommand refresh;
    refresh.set_operation("refresh");
    refresh.set_scope("urls");
    refresh.set_policy_id("policy");
    refresh.add_website_ids("site");
    refresh.add_urls("http://example.com/a");
    FLEXEDGE_REQUIRE(second.invalidate(refresh, std::array{cache_invalidation_url{"site", *flexedge::cache::parse_url(refresh.urls(0))}}).first == 1);
    FLEXEDGE_REQUIRE(!store(first, *in_flight.ticket_, site.response_cache(), rule, value, {}));
    FLEXEDGE_REQUIRE(*retained->body_ == "body");
    auto miss = first.lookup(site, rule, "http://example.com/a", {}, false);
    FLEXEDGE_REQUIRE(!miss.response_);

    for (const auto& control : {"private", "no-store"}) {
        auto origin_rule = rule;
        origin_rule.ttl_mode_ = "origin";
        auto ticket = first.lookup(site, rule, "http://example.com/private", {});
        auto private_response = response();
        private_response.headers_.emplace_back("Cache-Control", control);
        FLEXEDGE_REQUIRE(!store(first, *ticket.ticket_, site.response_cache(), origin_rule, private_response, {}));
    }
    FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(rule, "GET", {{"Cookie", "sid=secret"}}) == "request_cookie");
    FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(rule, "GET", {{"Authorization", "secret"}}).empty());
    FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(rule, "GET", {{"Range", "bytes=0-3"}}).empty());
    {
        auto configurable = rule;
        configurable.skip_no_cache_ = false;
        configurable.skip_request_cookie_ = false;
        configurable.respect_request_no_cache_ = false;
        FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(configurable, "GET", {{"Cookie", "sid=secret"}}).empty());
        FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(configurable, "HEAD", {{"Cookie", "sid=secret"}, {"Cache-Control", "no-cache"}}).empty());
        FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(configurable, "GET", {{"Cookie", "sid=secret"}, {"Authorization", "secret"}}).empty());
        FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(configurable, "GET", {{"Cookie", "sid=secret"}, {"Cache-Control", "no-store"}}) == "request_no_store");
        auto wire = site.response_cache();
        flexedge::cache::policy configured_policy;
        configured_policy.rules_.push_back(configurable);
        wire.clear_rules();
        flexedge::cache::to_proto(configured_policy, wire);
        const auto decoded = flexedge::cache::from_proto(wire).rules_.front();
        FLEXEDGE_REQUIRE(!decoded.skip_request_cookie_ && !decoded.respect_request_no_cache_);
        auto origin_no_cache = response();
        origin_no_cache.headers_.emplace_back("Cache-Control", "no-cache");
        auto ticket = first.lookup(site, configurable, "http://example.com/configured", {});
        FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, site.response_cache(), configurable, origin_no_cache, {}));
        const auto configured = second.lookup(site, configurable, "http://example.com/configured", {});
        FLEXEDGE_REQUIRE(configured.response_);
        first.discard(configured.key_, configured.response_->identity_);
        for (const auto control : {"private", "no-store"}) {
            origin_no_cache.headers_ = {{"Cache-Control", control}};
            const auto url = std::string("http://example.com/override-") + control;
            auto allowed = first.lookup(site, configurable, url, {});
            FLEXEDGE_REQUIRE(store(first, *allowed.ticket_, site.response_cache(), configurable, origin_no_cache, {}));
            FLEXEDGE_REQUIRE(first.lookup(site, configurable, url, {}, false).response_);
        }
        auto legacy = site.response_cache();
        legacy.mutable_rules(0)->clear_skip_no_cache();
        legacy.mutable_rules(0)->clear_respect_request_no_cache();
        legacy.mutable_rules(0)->clear_skip_request_cookie();
        const auto defaults = flexedge::cache::from_proto(legacy).rules_.front();
        FLEXEDGE_REQUIRE(defaults.skip_no_cache_ && !defaults.respect_request_no_cache_ && !defaults.skip_request_cookie_);
    }
    auto variant = first.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "en"}});
    auto varied = response("english");
    varied.headers_.emplace_back("Vary", "Accept-Language");
    FLEXEDGE_REQUIRE(store(first, *variant.ticket_, site.response_cache(), rule, varied, {{"Accept-Language", "en"}}));
    FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "en"}}, false).response_);
    FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "zh"}}, false).response_);
    {
        auto origin_rule = rule;
        origin_rule.ttl_mode_ = "origin";
        auto validated = response("validated");
        validated.headers_.emplace_back("Cache-Control", "no-cache");
        validated.headers_.emplace_back("ETag", "\"no-cache\"");
        const std::string url = "http://example.com/requires-validation";
        auto ticket = first.lookup(site, origin_rule, url, {});
        FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, site.response_cache(), origin_rule, validated, {}));
        auto stale = first.lookup(site, origin_rule, url, {});
        FLEXEDGE_REQUIRE(!stale.response_ && stale.stale_ && stale.ticket_);
        FLEXEDGE_REQUIRE(!response_cache::usable_stale(*stale.stale_));
        const auto fresh = first.revalidate(*stale.ticket_, origin_rule, *stale.stale_, {304, {}}, {});
        FLEXEDGE_REQUIRE(fresh && *fresh->body_ == "validated");
        FLEXEDGE_REQUIRE(!first.lookup(site, origin_rule, url, {}, false).response_);
        auto unspecified = response("fallback");
        unspecified.headers_.clear();
        auto fallback = first.lookup(site, origin_rule, "http://example.com/fallback", {});
        FLEXEDGE_REQUIRE(store(first, *fallback.ticket_, site.response_cache(), origin_rule, unspecified, {}));
        FLEXEDGE_REQUIRE(first.lookup(site, origin_rule, "http://example.com/fallback", {}, false).response_);
    }
    {
        auto cookie_rule = rule;
        cookie_rule.skip_request_cookie_ = false;
        cookie_rule.settings_.emplace();
        cookie_rule.settings_->enabled_ = {"vary"};
        cookie_rule.settings_->vary_default_ = "passthrough";
        const cache_headers first_cookie{{"Cookie", "session=first"}};
        auto ticket = first.lookup(site, cookie_rule, "http://example.com/cookie-vary.js", first_cookie);
        auto personalized = response("first");
        personalized.headers_.emplace_back("Vary", "Cookie");
        FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, site.response_cache(), cookie_rule, personalized, first_cookie));
        FLEXEDGE_REQUIRE(second.lookup(site, cookie_rule, "http://example.com/cookie-vary.js", first_cookie, false).response_);
        FLEXEDGE_REQUIRE(!second.lookup(site, cookie_rule, "http://example.com/cookie-vary.js", {{"Cookie", "session=second"}}, false).response_);
        FLEXEDGE_REQUIRE(!second.lookup(site, cookie_rule, "http://example.com/cookie-vary.js", {}, false).response_);
        cookie_rule.ttl_mode_ = "origin";
        personalized.headers_.emplace_back("Set-Cookie", "session=first");
        FLEXEDGE_REQUIRE(response_cache::response_bypass_reason(cookie_rule, {200, personalized.headers_}) == "origin_set_cookie");
    }

    const auto entries_before_writers = publication->snapshot()->entries_.size();
    std::barrier start(4);
    std::vector<std::jthread> writers;
    std::atomic<int> failures{};
    for (int worker = 0; worker < 4; ++worker) {
        writers.emplace_back([&, worker] {
            response_cache local(publication);
            start.arrive_and_wait();
            try {
                for (int index = 0; index < 30; ++index) {
                    const auto url = "http://example.com/" + std::to_string(worker) + "/" + std::to_string(index);
                    auto ticket = local.lookup(site, rule, url, {});
                    auto body = response(url);
                    if (!ticket.ticket_ || !store(local, *ticket.ticket_, site.response_cache(), rule, body, {})) {
                        ++failures;
                    }
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    writers.clear();
    FLEXEDGE_REQUIRE(failures == 0);
    FLEXEDGE_REQUIRE(publication->snapshot()->entries_.size() == entries_before_writers + 120);

    auto disk_site = site;
    disk_site.mutable_response_cache()->set_storage("disk");
    auto disk = first.lookup(disk_site, rule, "http://example.com/disk", {});
    FLEXEDGE_REQUIRE(store(first, *disk.ticket_, disk_site.response_cache(), rule, value, {}));
    auto disk_object = second.lookup(disk_site, rule, "http://example.com/disk", {}, false).response_;
    FLEXEDGE_REQUIRE(disk_object && disk_object->file_ && std::filesystem::exists(disk_object->file_->path_));
    const auto file = disk_object->file_->path_;
    v2::CacheCommand clean;
    clean.set_operation("clean");
    clean.set_scope("all");
    clean.set_policy_id("policy");
    first.invalidate(clean, {});
    FLEXEDGE_REQUIRE(std::filesystem::exists(file));
    disk_object.reset();
    FLEXEDGE_REQUIRE(!std::filesystem::exists(file));
    {
        // Host-sensitive responses remain separate when the origin declares Vary.
        auto host_value = response("example.com");
        host_value.headers_.emplace_back("Vary", "Host");
        const auto pending = first.lookup(site, rule, "http://example.com/host", {{"Host", "example.com"}});
        FLEXEDGE_REQUIRE(store(first, *pending.ticket_, site.response_cache(), rule, host_value, {{"Host", "example.com"}}));
        FLEXEDGE_REQUIRE(!second.lookup(site, rule, "http://alias.example.com/host", {{"Host", "alias.example.com"}}, false).response_);
        FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://example.com/host", {{"Host", "example.com"}}, false).response_);
        // A URL resolves to exactly one website, even when a command includes both.
        const auto other = first.lookup(other_site, rule, "http://other.example/host", {});
        FLEXEDGE_REQUIRE(store(first, *other.ticket_, other_site.response_cache(), rule, value, {}));
        refresh.add_website_ids(other_site.id());
        refresh.set_scope("prefix");
        const std::array urls{cache_invalidation_url{site.id(), *flexedge::cache::parse_url("http://example.com/ho")}};
        FLEXEDGE_REQUIRE(first.invalidate(refresh, urls).first == 1);
        FLEXEDGE_REQUIRE(second.lookup(other_site, rule, "http://other.example/host", {}, false).response_);
        first.invalidate(clean, {});
    }
    {
        auto tiered = site;
        tiered.mutable_response_cache()->set_storage("tiered");
        tiered.mutable_response_cache()->set_memory_bytes(2100);
        tiered.mutable_response_cache()->set_disk_bytes(2500);
        tiered.mutable_response_cache()->set_max_object_bytes(1024);
        const auto store_tiered = [&](int index) {
            const auto url = "http://example.com/tiered/" + std::to_string(index);
            auto ticket = first.lookup(tiered, rule, url, {});
            auto payload = response(std::string(512, static_cast<char>('a' + index)));
            FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, tiered.response_cache(), rule, payload, {}));
        };
        for (int i = 0; i < 3; ++i) {
            store_tiered(i);
        }
        const auto cold = second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_;
        FLEXEDGE_REQUIRE(cold && cold->file_ && !cold->body_);
        const auto expiry = cold->expires_at_;
        // Touch 1 before promoting 0: 2 becomes the least recently used L1 object.
        FLEXEDGE_REQUIRE(second.lookup(tiered, rule, "http://example.com/tiered/1", {}, false).response_->body_);
        auto lookup = second.lookup(tiered, rule, "http://example.com/tiered/0", {});
        FLEXEDGE_REQUIRE(lookup.response_ && !lookup.response_->body_);
        buffered_bytes_budget promotion_budget(1024);
        auto promotion = second.begin_promotion(*lookup.response_, tiered.response_cache(), promotion_budget.lease());
        FLEXEDGE_REQUIRE(promotion && promotion->append(std::string(256, 'a')));
        FLEXEDGE_REQUIRE(!second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_->body_);
        FLEXEDGE_REQUIRE(promotion->append(std::string(256, 'a')));
        second.promote(lookup.key_, lookup.response_, tiered.response_cache(), *promotion);
        promotion.reset();
        FLEXEDGE_REQUIRE(promotion_budget.used_bytes() == 0);
        auto hot = second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_;
        FLEXEDGE_REQUIRE(hot && hot->file_ && hot->body_ && *hot->body_ == std::string(512, 'a'));
        FLEXEDGE_REQUIRE(hot->expires_at_ == expiry && !cold->body_);
        // Inspecting 2 itself touches its LRU position, so inspect immutable roots here.
        const auto root = publication->snapshot();
        bool second_is_disk_only{};
        for (const auto& [key, items] : root->entries_) {
            (void)key;
            for (const auto& item : items) {
                if (item->url_ == "http://example.com/tiered/2") {
                    second_is_disk_only = item->file_ && !item->body_;
                }
            }
        }
        FLEXEDGE_REQUIRE(second_is_disk_only);
        store_tiered(3);
        FLEXEDGE_REQUIRE(!second.lookup(tiered, rule, "http://example.com/tiered/2", {}, false).response_);
        first.invalidate(clean, {});
        FLEXEDGE_REQUIRE(!second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_);
    }
    {
        // Disk filling must not retain an object-sized memory copy or publish a prefix.
        auto streamed = site;
        streamed.mutable_response_cache()->set_storage("disk");
        streamed.mutable_response_cache()->set_max_object_bytes(128 * 1024);
        const auto ticket = first.lookup(streamed, rule, "http://example.com/stream", {});
        buffered_bytes_budget budget(8);
        auto disk_fill = first.begin_fill(streamed.response_cache(), rule, {200, response().headers_}, budget.lease());
        FLEXEDGE_REQUIRE(disk_fill);
        const std::string chunk(16384, 's');
        for (int i = 0; i < 8; ++i) {
            FLEXEDGE_REQUIRE(disk_fill->append(chunk));
            FLEXEDGE_REQUIRE(disk_fill->memory_bytes() == 0 && budget.used_bytes() == 0);
            FLEXEDGE_REQUIRE(!second.lookup(streamed, rule, "http://example.com/stream", {}, false).response_);
        }
        bool partial_file{};
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            if (entry.path().extension() == ".tmp") {
                std::ifstream partial(entry.path(), std::ios::binary);
                std::string prefix(chunk.size(), '\0');
                partial.read(prefix.data(), static_cast<std::streamsize>(prefix.size()));
                partial_file = prefix == chunk;
            }
        }
        FLEXEDGE_REQUIRE(partial_file);
        FLEXEDGE_REQUIRE(first.store(*ticket.ticket_, streamed.response_cache(), rule, *disk_fill, {}));
        auto hit = second.lookup(streamed, rule, "http://example.com/stream", {}, false).response_;
        FLEXEDGE_REQUIRE(hit && hit->file_ && !hit->body_ && hit->body_bytes_ == 128 * 1024);
        std::ifstream input(hit->file_->path_, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(input)), {});
        FLEXEDGE_REQUIRE(bytes == std::string(128 * 1024, 's'));
        input.close();
        disk_fill.reset();
        auto aborted = first.begin_fill(streamed.response_cache(), rule, {200, response().headers_}, budget.lease());
        FLEXEDGE_REQUIRE(aborted && aborted->append(chunk));
        FLEXEDGE_REQUIRE(!aborted->append(std::string(128 * 1024, 'x')));
        aborted.reset();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            FLEXEDGE_REQUIRE(entry.path().extension() != ".tmp");
        }
        auto unfinished = first.begin_fill(streamed.response_cache(), rule, {200, response().headers_}, budget.lease());
        FLEXEDGE_REQUIRE(unfinished && unfinished->append(chunk));
        unfinished.reset();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            FLEXEDGE_REQUIRE(entry.path().extension() != ".tmp");
        }
        // Invalidation during a streamed promotion must not resurrect an object.
        auto tiered = streamed;
        tiered.mutable_response_cache()->set_storage("tiered");
        auto pending_promotion = second.begin_promotion(*hit, tiered.response_cache(), {});
        FLEXEDGE_REQUIRE(pending_promotion && pending_promotion->append(bytes));
        const auto pending_key = second.lookup(streamed, rule, "http://example.com/stream", {}, false).key_;
        first.invalidate(clean, {});
        second.promote(pending_key, hit, tiered.response_cache(), *pending_promotion);
        FLEXEDGE_REQUIRE(!second.lookup(streamed, rule, "http://example.com/stream", {}, false).response_);
        auto tiny_memory = first.begin_fill(site.response_cache(), rule, {200, response().headers_}, budget.lease());
        FLEXEDGE_REQUIRE(tiny_memory && !tiny_memory->append(chunk));
        FLEXEDGE_REQUIRE(budget.used_bytes() == 0);
        first.invalidate(clean, {});
    }
    {
        auto encoded = site;
        encoded.set_response_compression_enabled(true);
        encoded.add_response_compression_algorithms("br");
        encoded.add_response_compression_algorithms("gzip");
        const cache_headers original{{"Accept-Encoding", "gzip;q=1, br;q=0.5"}};
        const auto ticket = first.lookup(encoded, rule, "http://example.com/encoded", original);
        auto compressed = response("gzip representation");
        compressed.headers_.emplace_back("Content-Encoding", "gzip");
        compressed.headers_.emplace_back("Vary", "Accept-Encoding, Accept-Language");
        FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, encoded.response_cache(), rule, compressed, original));
        FLEXEDGE_REQUIRE(second.lookup(encoded, rule, "http://example.com/encoded",
                                   {{"Accept-Encoding", "BR;q=0.1, GZIP"}}, false)
                .response_);
        FLEXEDGE_REQUIRE(!second.lookup(encoded, rule, "http://example.com/encoded",
                                    {{"Accept-Encoding", "gzip;q=0, br"}}, false)
                .response_);
        FLEXEDGE_REQUIRE(second.lookup(encoded, rule, "http://example.com/encoded",
                                   {{"Accept-Encoding", "gzip"}, {"Accept-Language", "zh"}}, false)
                .response_);
        FLEXEDGE_REQUIRE(!second.lookup(encoded, rule, "http://example.com/encoded", {}, false).response_);
        first.invalidate(clean, {});
        FLEXEDGE_REQUIRE(!first.begin_fill(site.response_cache(), rule,
            {200, {{"Content-Type", "Text/Event-Stream; charset=utf-8"}}}, {}));
        FLEXEDGE_REQUIRE(response_cache::request_bypass_reason(rule, "GET", {{"Accept", "application/json, Text/Event-Stream"}}) == "request_event_stream");
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory / "cache")) {
        std::filesystem::remove(entry.path());
    }
    std::filesystem::remove(directory / "cache");
    std::filesystem::remove_all(directory);
    FLEXEDGE_REQUIRE(flexedge::cache::parse_url("http://EXAMPLE.com:80/a")->value_ == "http://example.com/a");
    FLEXEDGE_REQUIRE(!flexedge::cache::parse_url("http://user@example.com/"));
    const std::set<std::string> domains{"*.example.com", "example.org"};
    for (const auto address : {"https://ASSETS.Example.com:443/a", "http://example.org/a"}) {
        const auto url = flexedge::cache::parse_url(address);
        FLEXEDGE_REQUIRE(url && flexedge::route_policy::find_hostname(domains, url->hostname_) != domains.end());
    }
    for (const auto address : {"https://example.com/a", "https://a.b.example.com/a", "https://badexample.com/a"}) {
        const auto url = flexedge::cache::parse_url(address);
        FLEXEDGE_REQUIRE(url && flexedge::route_policy::find_hostname(domains, url->hostname_) == domains.end());
    }
    auto invalid = flexedge::cache::from_proto(site.response_cache());
    invalid.directory_ = "../escape";
    FLEXEDGE_REQUIRE(!flexedge::cache::validate(invalid).empty());
}
}  // namespace
int main() {
    try {
        directive_boundaries();
        cloudflare_defaults();
        optional_settings();
        expressions();
        verify();
        std::cout << "response cache invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
