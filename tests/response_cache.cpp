#include "node/data/response_cache.h"

#include <barrier>
#include <iostream>
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
buffered_proxy_response response(std::string body = "body") {
    buffered_proxy_response value;
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
    FLEXEDGE_REQUIRE(first.store(*fill.ticket_, site.response_cache(), rule, value, {}));
    const auto retained = second.lookup(site, rule, "http://example.com/a", {}).response_;
    FLEXEDGE_REQUIRE(retained && *retained->body_ == "body");
    auto in_flight = first.lookup(site, rule, "http://example.com/b", {});
    v2::CacheCommand refresh;
    refresh.set_operation("refresh");
    refresh.set_scope("urls");
    refresh.set_policy_id("policy");
    refresh.add_website_ids("site");
    refresh.add_urls("http://example.com/a");
    FLEXEDGE_REQUIRE(second.invalidate(refresh).first == 1);
    FLEXEDGE_REQUIRE(!first.store(*in_flight.ticket_, site.response_cache(), rule, value, {}));
    FLEXEDGE_REQUIRE(*retained->body_ == "body");
    auto miss = first.lookup(site, rule, "http://example.com/a", {}, false);
    FLEXEDGE_REQUIRE(!miss.response_);

    for (const auto& control : {"private", "no-store", "no-cache"}) {
        auto ticket = first.lookup(site, rule, "http://example.com/private", {});
        auto private_response = response();
        private_response.headers_.emplace_back("Cache-Control", control);
        FLEXEDGE_REQUIRE(!first.store(*ticket.ticket_, site.response_cache(), rule, private_response, {}));
    }
    FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Cookie", "sid=secret"}}));
    FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Authorization", "secret"}}));
    FLEXEDGE_REQUIRE(!first.eligible("GET", {{"Range", "bytes=0-3"}}));
    auto variant = first.lookup(site, rule, "http://example.com/vary", {{"Accept-Language", "en"}});
    auto varied = response("english");
    varied.headers_.emplace_back("Vary", "Accept-Language");
    FLEXEDGE_REQUIRE(first.store(*variant.ticket_, site.response_cache(), rule, varied, {{"Accept-Language", "en"}}));
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
                    if (!ticket.ticket_ || !local.store(*ticket.ticket_, site.response_cache(), rule, body, {})) {
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
    FLEXEDGE_REQUIRE(first.store(*disk.ticket_, disk_site.response_cache(), rule, value, {}));
    auto disk_object = second.lookup(disk_site, rule, "http://example.com/disk", {}, false).response_;
    FLEXEDGE_REQUIRE(disk_object && disk_object->file_ && std::filesystem::exists(disk_object->file_->path_));
    const auto file = disk_object->file_->path_;
    v2::CacheCommand clean;
    clean.set_operation("clean");
    clean.set_scope("all");
    clean.set_policy_id("policy");
    first.invalidate(clean);
    FLEXEDGE_REQUIRE(std::filesystem::exists(file));
    disk_object.reset();
    FLEXEDGE_REQUIRE(!std::filesystem::exists(file));
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
            FLEXEDGE_REQUIRE(first.store(*ticket.ticket_, tiered.response_cache(), rule, payload, {}));
        };
        for (int i = 0; i < 3; ++i) {
            store_tiered(i);
        }
        const auto cold = second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_;
        FLEXEDGE_REQUIRE(cold && cold->file_ && !cold->body_);
        const auto expiry = cold->expires_at_;
        // Touch 1 before promoting 0: 2 becomes the least recently used L1 object.
        FLEXEDGE_REQUIRE(second.lookup(tiered, rule, "http://example.com/tiered/1", {}, false).response_->body_);
        auto hot = second.lookup(tiered, rule, "http://example.com/tiered/0", {}).response_;
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
        first.invalidate(clean);
        FLEXEDGE_REQUIRE(!second.lookup(tiered, rule, "http://example.com/tiered/0", {}, false).response_);
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory / "cache")) {
        std::filesystem::remove(entry.path());
    }
    std::filesystem::remove(directory / "cache");
    std::filesystem::remove(directory);
    FLEXEDGE_REQUIRE(flexedge::cache::parse_url("http://EXAMPLE.com:80/a")->value_ == "http://example.com/a");
    FLEXEDGE_REQUIRE(!flexedge::cache::parse_url("http://user@example.com/"));
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
