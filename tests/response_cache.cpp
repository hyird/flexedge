#include "node/data/response_cache.h"

#include <barrier>
#include <iostream>
#include <set>
#include <thread>

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
    auto fill = cache.begin_fill(policy, rule, {value.status_, value.headers_}, {});
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
    FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://alias.example.com/a", {}, false).response_ == retained);
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
    refresh.add_urls("http://alias.example.com/a");
    FLEXEDGE_REQUIRE(second.invalidate(refresh, std::array{cache_invalidation_url{"site", *flexedge::cache::parse_url(refresh.urls(0))}}).first == 1);
    FLEXEDGE_REQUIRE(!store(first, *in_flight.ticket_, site.response_cache(), rule, value, {}));
    FLEXEDGE_REQUIRE(*retained->body_ == "body");
    auto miss = first.lookup(site, rule, "http://example.com/a", {}, false);
    FLEXEDGE_REQUIRE(!miss.response_);

    for (const auto& control : {"private", "no-store", "no-cache"}) {
        auto ticket = first.lookup(site, rule, "http://example.com/private", {});
        auto private_response = response();
        private_response.headers_.emplace_back("Cache-Control", control);
        FLEXEDGE_REQUIRE(!store(first, *ticket.ticket_, site.response_cache(), rule, private_response, {}));
    }
    FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Cookie", "sid=secret"}}));
    FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Authorization", "secret"}}));
    FLEXEDGE_REQUIRE(first.eligible("GET", {{"Range", "bytes=0-3"}}));
    {
        auto configurable = rule;
        configurable.skip_no_cache_ = false;
        auto origin_no_cache = response();
        origin_no_cache.headers_.emplace_back("Cache-Control", "no-cache");
        auto ticket = first.lookup(site, configurable, "http://example.com/configured", {});
        FLEXEDGE_REQUIRE(store(first, *ticket.ticket_, site.response_cache(), configurable, origin_no_cache, {}));
        const auto configured = second.lookup(site, configurable, "http://example.com/configured", {});
        FLEXEDGE_REQUIRE(configured.response_);
        first.discard(configured.key_, configured.response_->identity_);
        for (const auto control : {"private", "no-store"}) {
            origin_no_cache.headers_ = {{"Cache-Control", control}};
            auto blocked = first.lookup(site, configurable, "http://example.com/blocked", {});
            FLEXEDGE_REQUIRE(!store(first, *blocked.ticket_, site.response_cache(), configurable, origin_no_cache, {}));
        }
        auto legacy = site.response_cache();
        legacy.mutable_rules(0)->clear_skip_no_cache();
        legacy.mutable_rules(0)->clear_respect_request_no_cache();
        const auto defaults = flexedge::cache::from_proto(legacy).rules_.front();
        FLEXEDGE_REQUIRE(defaults.skip_no_cache_ && defaults.respect_request_no_cache_);
    }
    auto variant = first.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "en"}});
    auto varied = response("english");
    varied.headers_.emplace_back("Vary", "Accept-Language");
    FLEXEDGE_REQUIRE(store(first, *variant.ticket_, site.response_cache(), rule, varied, {{"Accept-Language", "en"}}));
    FLEXEDGE_REQUIRE(second.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "en"}}, false).response_);
    FLEXEDGE_REQUIRE(!second.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "zh"}}, false).response_);

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
    FLEXEDGE_REQUIRE(publication->snapshot()->entries_.size() == 121);

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
        const std::array urls{cache_invalidation_url{site.id(), *flexedge::cache::parse_url("http://alias.example.com/ho")}};
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
        FLEXEDGE_REQUIRE(!second.lookup(encoded, rule, "http://example.com/encoded",
                                    {{"Accept-Encoding", "gzip"}, {"Accept-Language", "zh"}}, false)
                .response_);
        FLEXEDGE_REQUIRE(!second.lookup(encoded, rule, "http://example.com/encoded", {}, false).response_);
        first.invalidate(clean, {});
        FLEXEDGE_REQUIRE(!first.begin_fill(site.response_cache(), rule,
            {200, {{"Content-Type", "Text/Event-Stream; charset=utf-8"}}}, {}));
        FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Accept", "application/json, Text/Event-Stream"}}));
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
        verify();
        std::cout << "response cache invariants passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
