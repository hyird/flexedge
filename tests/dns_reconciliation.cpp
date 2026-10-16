#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "service/features/dns_sync/reconciliation.h"
#include "service/features/dns_sync/snapshot.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

using namespace service::dns_sync;
using namespace service::dns_sync::detail;
using namespace service::dns;

int main() {
    // Cloudflare names records fully qualified, Aliyun relative to the zone.
    const record_name_policy cloudflare(true);
    const record_name_policy aliyun(false);
    REQUIRE(cloudflare.remote_record_name("@", "example.com") == "example.com");
    REQUIRE(cloudflare.local_record_name("www.example.com.", "example.com") == "www");
    REQUIRE(aliyun.remote_record_name("www.example.com", "example.com") == "www");
    REQUIRE(aliyun.local_record_name("", "example.com") == "@");

    zone_config_data config{{{.id_ = "local-1", .type_ = "A", .name_ = "www", .content_ = "192.0.2.1", .ttl_ = 300, .priority_ = {}, .proxied_ = false, .line_code_ = "default"}}};
    zone_runtime_data previous;
    previous.record_states_.push_back({.id_ = "local-1", .remote_record_id_ = "remote-1", .sync_status_ = "synced", .synced_revision_ = 1, .last_error_ = {}, .managed_ = false});
    const service::dns::provider_record remote{.id_ = "remote-1", .type_ = "A", .name_ = "www.example.com", .content_ = "192.0.2.2", .ttl_ = 600, .priority_ = {}, .proxied_ = false, .line_code_ = "default"};
    const std::vector<service::dns::provider_line> lines{{"default", "Default", "Default"}};
    const auto observed = capture_zone_snapshot("zone", "example.com", cloudflare, {remote}, lines, previous, {}, 1);
    REQUIRE(observed.records_.records_.size() == 1);
    REQUIRE(observed.records_.records_[0].id_ == "local-1");
    REQUIRE(observed.records_.records_[0].content_ == "192.0.2.2");
    REQUIRE(plan_record_changes(config, config).writes_.records_.empty());
    auto edited = config;
    edited.records_[0].content_ = "192.0.2.3";
    REQUIRE(plan_record_changes(config, edited).writes_.records_.size() == 1);
    REQUIRE(plan_record_changes(config, {}).removals_.records_.size() == 1);
    const auto removed = capture_zone_snapshot("zone", "example.com", cloudflare, {}, lines, previous, {}, 1);
    REQUIRE(removed.records_.records_.empty());
    const std::vector<zone_record_runtime_data> owned{{.id_ = "system", .remote_record_id_ = "remote-1", .sync_status_ = "synced", .synced_revision_ = 1, .last_error_ = {}, .managed_ = true}};
    const auto system = capture_zone_snapshot("zone", "example.com", cloudflare, {remote}, lines, previous, owned, 1);
    REQUIRE(system.records_.records_.empty());
    REQUIRE(system.runtime_.record_states_[0].managed_);
    auto unknown = remote;
    unknown.type_ = "CAA";
    const auto ignored = capture_zone_snapshot("zone", "example.com", cloudflare, {unknown}, lines, {}, {}, 1);
    REQUIRE(ignored.records_.records_.empty());
    REQUIRE(snapshot_record_id("zone", "remote") == snapshot_record_id("zone", "remote"));
    REQUIRE(snapshot_record_id("zone", "remote") != snapshot_record_id("another", "remote"));
    bool duplicate_rejected = false;
    try {
        (void)capture_zone_snapshot("zone", "example.com", cloudflare, {remote, remote}, lines, previous, {}, 1);
    } catch (const std::runtime_error&) {
        duplicate_rejected = true;
    }
    REQUIRE(duplicate_rejected);

    // A remote record shared by two projected nodes survives when only one
    // node is removed. Unowned records are never deleted by their old UUID.
    zone_runtime_data shared;
    shared.record_states_ = owned;
    shared.record_states_.push_back({.id_ = "remaining", .remote_record_id_ = "remote-1", .sync_status_ = "synced", .synced_revision_ = 1, .last_error_ = {}, .managed_ = true});
    auto retained = config;
    retained.records_[0].id_ = "remaining";
    REQUIRE(obsolete_system_record_ids(shared, retained, {remote}, {}, cloudflare, "example.com").empty());
    REQUIRE(obsolete_system_record_ids(previous, {}, {remote}, {}, cloudflare, "example.com").empty());
    REQUIRE(obsolete_system_record_ids(shared, {}, {remote}, {}, cloudflare, "example.com").contains("remote-1"));
    REQUIRE(obsolete_system_record_ids({}, config, {remote}, {"www.example.com"}, cloudflare, "example.com").contains("remote-1"));
    REQUIRE(obsolete_system_record_ids({}, config, {unknown}, {"www.example.com"}, cloudflare, "example.com").empty());

    // A hosted apex domain projects its CNAME under the zone's own name; each provider gets
    // its own spelling of the apex and every spelling reads back as "@".
    REQUIRE(cloudflare.remote_record_name("example.com", "example.com") == "example.com");
    REQUIRE(aliyun.remote_record_name("example.com", "example.com") == "@");
    REQUIRE(aliyun.remote_record_name("EXAMPLE.COM.", "example.com") == "@");
    REQUIRE(cloudflare.local_record_name("example.com", "example.com") == "@");
    REQUIRE(aliyun.local_record_name("@", "example.com") == "@");
    REQUIRE(cloudflare.remote_record_name("*.example.com", "example.com") == "*.example.com");
    REQUIRE(aliyun.remote_record_name("*.example.com", "example.com") == "*");
    REQUIRE(aliyun.remote_record_name("_acme-challenge.example.com", "example.com") == "_acme-challenge");

    auto record = [](std::string id, std::string type, std::string name, std::string content,
                      std::string line = "default") {
        return zone_record_data{.id_ = std::move(id),
            .type_ = std::move(type),
            .name_ = std::move(name),
            .content_ = std::move(content),
            .ttl_ = 600,
            .priority_ = {},
            .proxied_ = false,
            .line_code_ = std::move(line)};
    };
    const std::unordered_set<std::string> websites{"apex", "www"};
    const auto apex = record("apex", "CNAME", "example.com", "edge.example.net");
    const auto www = record("www", "CNAME", "www.example.com", "edge.example.net");
    for (const auto* names : {&cloudflare, &aliyun}) {
        // MX, TXT and the certificate challenge stay beside an apex CNAME.
        zone_config_data desired{{apex, www, record("mx", "MX", "@", "mx.example.org"),
            record("spf", "TXT", "@", "v=spf1 -all"),
            record("acme", "TXT", "_acme-challenge.example.com", "token"),
            record("same", "CNAME", "@", "EDGE.example.net.")}};
        REQUIRE(find_blocked_website_records(desired, websites, *names, "example.com").empty());

        // A, AAAA or another CNAME at the apex hold the website CNAME back; nothing else is touched.
        desired.records_.push_back(record("a", "A", "@", "192.0.2.1"));
        desired.records_.push_back(record("aaaa", "AAAA", "@", "2001:db8::1"));
        desired.records_.push_back(record("a-2", "A", "@", "192.0.2.2"));
        auto blocked = find_blocked_website_records(desired, websites, *names, "example.com");
        REQUIRE(blocked.size() == 1);
        REQUIRE(blocked[0].id_ == "apex" && blocked[0].name_ == "@" && blocked[0].type_ == "CNAME");
        REQUIRE(blocked[0].error_ ==
                "主机记录 @ 已有 A、AAAA 记录，不能再添加 CNAME；请删除或修改这些记录，或把该网站域名改为外部解析");
        REQUIRE(unsynced_records_error(blocked) == "1 条记录未同步：@ CNAME：" + blocked[0].error_);

        // Below the apex nothing shares a name with a CNAME, TXT included.
        desired.records_.push_back(record("www-txt", "TXT", "www", "verify"));
        blocked = find_blocked_website_records(desired, websites, *names, "example.com");
        REQUIRE(blocked.size() == 2 && blocked[1].id_ == "www" && blocked[1].name_ == "www");
        REQUIRE(unsynced_records_error(blocked).starts_with("2 条记录未同步，首条 @ CNAME："));

        // Records on another resolution line, or user records that are not website CNAMEs,
        // never hold anything back.
        const zone_config_data other_line{{apex, record("telecom", "A", "@", "192.0.2.9", "telecom"),
            record("user", "CNAME", "blog", "elsewhere.example.org"), record("blog-a", "TXT", "blog", "x")}};
        REQUIRE(find_blocked_website_records(other_line, websites, *names, "example.com").empty());
    }
    REQUIRE(unsynced_records_error({}).empty());
}
