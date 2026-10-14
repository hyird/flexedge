#include <stdexcept>

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
    const record_name_policy cloudflare(dns_provider_kind::cloudflare);
    const record_name_policy aliyun(dns_provider_kind::aliyun);
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
}
