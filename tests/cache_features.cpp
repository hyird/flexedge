#include <fstream>
#include <iostream>

#include "node/data/response_cache.h"

#define FLEXEDGE_REQUIRE(value)                                            \
    do {                                                                   \
        if (!(value)) throw std::runtime_error("cache features: " #value); \
    } while (false)
using namespace flexedge::node;
namespace {
struct directory final {
    std::filesystem::path path_ = std::filesystem::current_path() / ("cache-features-" + std::to_string(std::random_device{}()));
    ~directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
};
v2::Website website(std::string_view storage = "memory") {
    v2::Website result;
    result.set_id("site");
    result.set_revision(1);
    flexedge::cache::policy policy;
    policy.id_ = "policy";
    policy.storage_ = storage;
    flexedge::cache::rule rule;
    rule.id_ = "all";
    rule.name_ = "all";
    rule.match_type_ = "all";
    rule.ttl_mode_ = "fixed";
    rule.ttl_seconds_ = 600;
    policy.rules_.push_back(rule);
    flexedge::cache::to_proto(policy, *result.mutable_response_cache());
    return result;
}
const std::string object_url = "http://example.com/file";
std::shared_ptr<const cached_response> insert(response_cache& cache, const v2::Website& site, const flexedge::cache::rule& rule,
    std::string_view control = "public, max-age=600, stale-if-error=120", const std::string& address = object_url,
    std::string_view bytes = "abcdefghij") {
    const auto ticket = cache.lookup(site, rule, address, {});
    FLEXEDGE_REQUIRE(ticket.ticket_);
    auto fill = cache.begin_fill(site.response_cache(), rule,
        {200, {{"Content-Type", "text/plain"}, {"Cache-Control", std::string(control)}, {"ETag", "\"etag\""}}}, {});
    FLEXEDGE_REQUIRE(fill && fill->append(bytes));
    FLEXEDGE_REQUIRE(cache.store(*ticket.ticket_, site.response_cache(), rule, *fill, {}));
    return cache.lookup(site, rule, address, {}, false).response_;
}
void expire(const std::shared_ptr<response_cache_publication>& publication, const std::string& key) {
    auto root = publication->snapshot();
    auto next = std::make_shared<response_cache_snapshot>(*root);
    auto copy = std::make_shared<cached_response>(*next->entries_.at(key).front());
    copy->expires_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    next->entries_[key][0] = copy;
    FLEXEDGE_REQUIRE(publication->publish(root, next));
}
void verify_large_objects() {
    auto policy = flexedge::cache::policy{};
    FLEXEDGE_REQUIRE(policy.max_object_bytes_ == 512 * 1024 * 1024);
    policy.max_object_bytes_ = 512 * 1024 * 1024;
    FLEXEDGE_REQUIRE(flexedge::cache::validate(policy).empty());
    ++policy.max_object_bytes_;
    FLEXEDGE_REQUIRE(!flexedge::cache::validate(policy).empty());

    directory files;
    auto site = website("disk");
    site.mutable_response_cache()->set_max_object_bytes(256 * 1024 * 1024);
    const auto rule = flexedge::cache::from_proto(site.response_cache()).rules_.front();
    constexpr std::size_t body_bytes = 65 * 1024 * 1024;
    const std::string chunk(64 * 1024, 'L');
    {
        auto publication = std::make_shared<response_cache_publication>(files.path_);
        response_cache writer(publication);
        const auto ticket = writer.lookup(site, rule, object_url, {});
        FLEXEDGE_REQUIRE(ticket.ticket_);
        buffered_bytes_budget budget(8);
        auto fill = writer.begin_fill(site.response_cache(), rule,
            {200, {{"Content-Type", "application/octet-stream"}}}, budget.lease());
        FLEXEDGE_REQUIRE(fill);
        for (std::size_t written = 0; written < body_bytes; written += chunk.size()) {
            FLEXEDGE_REQUIRE(fill->append(chunk));
            FLEXEDGE_REQUIRE(fill->memory_bytes() == 0 && budget.used_bytes() == 0);
        }
        FLEXEDGE_REQUIRE(writer.store(*ticket.ticket_, site.response_cache(), rule, *fill, {}));
    }
    {
        auto publication = std::make_shared<response_cache_publication>(files.path_);
        response_cache reader(publication);
        const auto hit = reader.lookup(site, rule, object_url, {}, false).response_;
        FLEXEDGE_REQUIRE(hit && hit->file_ && !hit->body_ && hit->body_bytes_ == body_bytes);
        std::ifstream input(hit->file_->path_, std::ios::binary);
        std::string bytes(chunk.size(), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        FLEXEDGE_REQUIRE(bytes == chunk);
        input.seekg(static_cast<std::streamoff>(body_bytes - chunk.size()));
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        FLEXEDGE_REQUIRE(bytes == chunk);
    }
}
void verify() {
    directory files;
    auto site = website();
    const auto rule = flexedge::cache::from_proto(site.response_cache()).rules_.front();
    auto publication = std::make_shared<response_cache_publication>(files.path_);
    response_cache cache(publication), other(publication);
    insert(cache, site, rule);
    const auto key = cache.lookup(site, rule, object_url, {}, false).key_;
    const auto before = publication->snapshot();
    for (int i = 0; i < 32; ++i) {
        FLEXEDGE_REQUIRE(cache.lookup(site, rule, object_url, {}, false).response_);
    }
    FLEXEDGE_REQUIRE(publication->snapshot() == before);
    for (int i = 0; i < 128; ++i) {
        FLEXEDGE_REQUIRE(cache.lookup(site, rule, object_url, {}, false).response_);
    }
    FLEXEDGE_REQUIRE(publication->snapshot() != before);

    expire(publication, key);
    const auto expired = cache.lookup(site, rule, object_url, {});
    FLEXEDGE_REQUIRE(expired.stale_ && expired.ticket_ && response_cache::usable_stale(*expired.stale_));
    FLEXEDGE_REQUIRE(other.lookup(site, rule, object_url, {}).filling_);
    FLEXEDGE_REQUIRE(cache.renew(*expired.ticket_));
    FLEXEDGE_REQUIRE(publication->snapshot()->filling_.at(key).expires_at_ > std::chrono::steady_clock::now() + std::chrono::seconds(20));
    const auto fresh = cache.revalidate(*expired.ticket_, rule, *expired.stale_,
        {304, {{"Cache-Control", "public, max-age=900"}, {"ETag", "\"etag\""}}}, {});
    FLEXEDGE_REQUIRE(fresh && fresh->body_ == expired.stale_->body_ && fresh->expires_at_ > std::chrono::steady_clock::now());
    FLEXEDGE_REQUIRE(other.lookup(site, rule, object_url, {}).response_);
    FLEXEDGE_REQUIRE(!cache.renew(*expired.ticket_));

    const auto orphan = cache.lookup(site, rule, object_url + "?lease", {});
    {
        auto root = publication->snapshot();
        auto next = std::make_shared<response_cache_snapshot>(*root);
        next->filling_[orphan.ticket_->key_].expires_at_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        FLEXEDGE_REQUIRE(publication->publish(root, next));
    }
    const auto successor = other.lookup(site, rule, object_url + "?lease", {});
    FLEXEDGE_REQUIRE(successor.ticket_ && successor.ticket_->claim_ != orphan.ticket_->claim_);
    FLEXEDGE_REQUIRE(!cache.renew(*orphan.ticket_));
    other.abandon(*successor.ticket_);
    const auto pending = cache.lookup(site, rule, object_url + "?pending", {});
    v2::CacheCommand clear;
    clear.set_operation("clean");
    clear.set_policy_id("policy");
    clear.set_scope("all");
    cache.invalidate(clear, {});
    FLEXEDGE_REQUIRE(!cache.renew(*pending.ticket_));
    FLEXEDGE_REQUIRE(!cache.current(key, *fresh));
    auto origin_rule = rule;
    origin_rule.ttl_mode_ = "origin";
    auto strict = insert(cache, site, origin_rule, "public, max-age=600, must-revalidate");
    FLEXEDGE_REQUIRE(!response_cache::usable_stale(*strict));

    const auto range = parse_cache_range("bytes=2-5", 10);
    FLEXEDGE_REQUIRE(range && range->satisfiable_ && range->offset_ == 2 && range->length_ == 4);
    FLEXEDGE_REQUIRE(parse_cache_range("bytes=-3", 10)->offset_ == 7);
    FLEXEDGE_REQUIRE(parse_cache_range("bytes=7-", 10)->length_ == 3);
    FLEXEDGE_REQUIRE(!parse_cache_range("bytes=99-", 10)->satisfiable_);
    FLEXEDGE_REQUIRE(!parse_cache_range("bytes=0-1,4-5", 10));
    FLEXEDGE_REQUIRE(!parse_cache_range("bytes=0-", 0)->satisfiable_);

    directory disk_files;
    site = website("disk");
    std::filesystem::path body;
    {
        auto disk = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache writer(disk);
        const auto value = insert(writer, site, rule);
        body = value->file_->path_;
    }
    FLEXEDGE_REQUIRE(std::filesystem::exists(body));
    {
        auto recovered = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache reader(recovered);
        const auto hit = reader.lookup(site, rule, object_url, {}, false);
        FLEXEDGE_REQUIRE(hit.response_ && hit.response_->file_ && hit.response_->body_bytes_ == 10 && !hit.response_->body_);
        auto metadata_path = hit.response_->file_->path_;
        metadata_path.replace_extension(".meta");
        std::ifstream old_metadata(metadata_path, std::ios::binary);
        const std::string previous_record((std::istreambuf_iterator<char>(old_metadata)), {});
        old_metadata.close();
        reader.invalidate(clear, {});
        // Simulate an old writer's metadata arriving after a completed purge.
        std::ofstream delayed(metadata_path, std::ios::binary | std::ios::trunc);
        delayed.write(previous_record.data(), static_cast<std::streamsize>(previous_record.size()));
        delayed.close();
        auto restart = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache restarted(restart);
        FLEXEDGE_REQUIRE(!restarted.lookup(site, rule, object_url, {}, false).response_);
    }
    {
        auto recovered = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache reader(recovered);
        FLEXEDGE_REQUIRE(!reader.lookup(site, rule, object_url, {}, false).response_);
    }
    {
        auto disk = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache writer(disk);
        insert(writer, site, rule);
        insert(writer, site, rule, "public, max-age=600", object_url + "/other");
        auto selective = clear;
        selective.set_operation("refresh");
        selective.set_scope("urls");
        selective.add_website_ids("site");
        selective.add_urls(object_url);
        writer.invalidate(selective, std::array{cache_invalidation_url{"site", *flexedge::cache::parse_url(selective.urls(0))}});
    }
    {
        auto recovered = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache reader(recovered);
        FLEXEDGE_REQUIRE(!reader.lookup(site, rule, object_url, {}, false).response_);
        FLEXEDGE_REQUIRE(reader.lookup(site, rule, object_url + "/other", {}, false).response_);
        reader.invalidate(clear, {});
    }
    // A truncated sidecar cannot make its body visible after a restart.
    {
        auto disk = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache writer(disk);
        body = insert(writer, site, rule)->file_->path_;
    }
    auto metadata = body;
    metadata.replace_extension(".meta");
    {
        std::ofstream broken(metadata, std::ios::binary | std::ios::trunc);
        broken << "bad";
    }
    {
        auto recovered = std::make_shared<response_cache_publication>(disk_files.path_);
        response_cache reader(recovered);
        FLEXEDGE_REQUIRE(!reader.lookup(site, rule, object_url, {}, false).response_);
        FLEXEDGE_REQUIRE(!std::filesystem::exists(body));
    }

    directory eviction_files;
    site.mutable_response_cache()->set_disk_bytes(500);
    site.mutable_response_cache()->set_max_object_bytes(10);
    {
        auto disk = std::make_shared<response_cache_publication>(eviction_files.path_);
        response_cache writer(disk);
        const auto pinned = insert(writer, site, rule);
        body = pinned->file_->path_;
        auto sidecar = body;
        sidecar.replace_extension(".meta");
        insert(writer, site, rule, "public, max-age=600", object_url + "/new");
        // Capacity eviction retires metadata immediately, while an active reader
        // still pins the old body until it has finished sending it.
        FLEXEDGE_REQUIRE(!std::filesystem::exists(sidecar) && std::filesystem::exists(body));
    }
    FLEXEDGE_REQUIRE(!std::filesystem::exists(body));
    {
        auto disk = std::make_shared<response_cache_publication>(eviction_files.path_);
        response_cache reader(disk);
        FLEXEDGE_REQUIRE(!reader.lookup(site, rule, object_url, {}, false).response_);
        FLEXEDGE_REQUIRE(reader.lookup(site, rule, object_url + "/new", {}, false).response_);
    }

    directory tiered_files;
    site = website("tiered");
    site.mutable_response_cache()->set_memory_bytes(1800);
    const std::string large_body(1024, 'x');
    {
        auto disk = std::make_shared<response_cache_publication>(tiered_files.path_);
        response_cache writer(disk);
        insert(writer, site, rule, "public, max-age=600", object_url, large_body);
        insert(writer, site, rule, "public, max-age=600", object_url + "/new", large_body);
        auto old = writer.lookup(site, rule, object_url, {}, false);
        FLEXEDGE_REQUIRE(old.response_ && !old.response_->body_);
        auto promoted = writer.begin_promotion(*old.response_, site.response_cache(), {});
        FLEXEDGE_REQUIRE(promoted && promoted->append(large_body));
        writer.promote(old.key_, old.response_, site.response_cache(), *promoted);
        FLEXEDGE_REQUIRE(writer.lookup(site, rule, object_url, {}, false).response_->body_);
        FLEXEDGE_REQUIRE(!writer.lookup(site, rule, object_url + "/new", {}, false).response_->body_);
    }
    {
        auto disk = std::make_shared<response_cache_publication>(tiered_files.path_);
        response_cache reader(disk);
        // Memory demotion and promotion must preserve both persistent bodies.
        FLEXEDGE_REQUIRE(reader.lookup(site, rule, object_url, {}, false).response_);
        FLEXEDGE_REQUIRE(reader.lookup(site, rule, object_url + "/new", {}, false).response_);
    }
}
}  // namespace
int main() {
    try {
        verify_large_objects();
        verify();
        std::cout << "cache features passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
