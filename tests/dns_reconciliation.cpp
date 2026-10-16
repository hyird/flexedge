#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include "service/features/dns_sync/reconciliation.h"

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

    const zone_config_data config{{{
        .id_ = "local-1",
        .type_ = "A",
        .name_ = "www",
        .content_ = "192.0.2.1",
        .ttl_ = 300,
        .priority_ = {},
        .proxied_ = false,
        .line_code_ = "default",
    }}};
    zone_runtime_data runtime;
    runtime.records_imported_ = true;
    zone_record_runtime_data state;
    state.id_ = "local-1";
    state.remote_record_id_ = "remote-1";
    runtime.record_states_.push_back(state);
    provider_line line{};
    line.code_ = "default";
    const std::vector<provider_line> lines{line};
    const provider_record remote{
        .id_ = "remote-1",
        .type_ = "A",
        .name_ = "www.example.com",
        .content_ = "192.0.2.2",
        .ttl_ = 600,
        .priority_ = {},
        .proxied_ = false,
        .line_code_ = "default",
    };
    auto plan = [&](std::string_view operation, std::vector<provider_record> records) {
        return plan_remote_merge(operation, config, runtime, config, "example.com",
            cloudflare, records, lines);
    };

    // A changed remote value requires an explicit resolution before a normal sync.
    const auto conflict = plan("sync", {remote});
    REQUIRE(conflict.conflicts_.size() == 1);
    REQUIRE(conflict.conflicts_[0].local_content_ == "192.0.2.1");
    REQUIRE(conflict.conflicts_[0].remote_content_ == "192.0.2.2");
    REQUIRE(conflict.records_.empty());
    REQUIRE(!conflict.changed_);

    const auto imported = plan("sync_remote", {remote});
    REQUIRE(imported.changed_ && imported.conflicts_.empty());
    REQUIRE(imported.records_.size() == 1);
    REQUIRE(imported.records_[0].id_ == "local-1");
    REQUIRE(imported.records_[0].name_ == "www");
    REQUIRE(imported.records_[0].content_ == "192.0.2.2");
    REQUIRE(imported.records_[0].ttl_ == 600);
    REQUIRE(imported.remote_ids_by_local_id_.at("local-1") == "remote-1");

    REQUIRE(plan("sync", {}).conflicts_.size() == 1);
    const auto deleted = plan("sync_remote", {});
    REQUIRE(deleted.changed_ && deleted.records_.empty());
    REQUIRE(deleted.remote_ids_by_local_id_.empty());
    const auto retained = plan("sync_local", {});
    REQUIRE(retained.records_.size() == 1);
    REQUIRE(retained.records_[0].content_ == "192.0.2.1");

    // Identical snapshots produce no write plan, including provider FQDN conversion.
    auto identical = remote;
    identical.content_ = "192.0.2.1";
    identical.ttl_ = 300;
    const auto unchanged = plan("sync", {identical});
    REQUIRE(!unchanged.changed_ && unchanged.conflicts_.empty());
    REQUIRE(unchanged.records_.size() == 1);

    bool duplicate_rejected = false;
    try {
        (void)plan("sync_remote", {remote, remote});
    } catch (const std::runtime_error&) {
        duplicate_rejected = true;
    }
    REQUIRE(duplicate_rejected);

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
