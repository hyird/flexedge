#include <stdexcept>
#include "service/features/dns_sync/reconciliation.h"

#define REQUIRE(condition) do { if (!(condition)) \
    throw std::runtime_error("requirement failed: " #condition); } while (false)

using namespace service::dns_sync;
using namespace service::dns_sync::detail;
using namespace service::dns;

int main() {
    const RecordNamePolicy cloudflare(DnsProviderKind::cloudflare);
    const RecordNamePolicy aliyun(DnsProviderKind::aliyun);
    REQUIRE(cloudflare.remoteRecordName("@", "example.com") == "example.com");
    REQUIRE(cloudflare.localRecordName("www.example.com.", "example.com") == "www");
    REQUIRE(aliyun.remoteRecordName("www.example.com", "example.com") == "www");
    REQUIRE(aliyun.localRecordName("", "example.com") == "@");

    const ZoneConfigData config{{{
        .id = "local-1", .type = "A", .name = "www", .content = "192.0.2.1",
        .ttl = 300, .priority = {}, .proxied = false, .lineCode = "default",
    }}};
    ZoneRuntimeData runtime;
    runtime.recordsImported = true;
    ZoneRecordRuntimeData state;
    state.id = "local-1";
    state.remoteRecordId = "remote-1";
    runtime.recordStates.push_back(state);
    ProviderLine line{};
    line.code = "default";
    const std::vector<ProviderLine> lines{line};
    const ProviderRecord remote{
        .id = "remote-1", .type = "A", .name = "www.example.com",
        .content = "192.0.2.2", .ttl = 600, .priority = {},
        .proxied = false, .lineCode = "default",
    };
    auto plan = [&](std::string_view operation, std::vector<ProviderRecord> records) {
        return planRemoteMerge(operation, config, runtime, config, "example.com",
                               cloudflare, records, lines);
    };

    // A changed remote value requires an explicit resolution before a normal sync.
    const auto conflict = plan("sync", {remote});
    REQUIRE(conflict.conflicts.size() == 1);
    REQUIRE(conflict.conflicts[0].localContent == "192.0.2.1");
    REQUIRE(conflict.conflicts[0].remoteContent == "192.0.2.2");
    REQUIRE(conflict.records.empty());
    REQUIRE(!conflict.changed);

    const auto imported = plan("sync_remote", {remote});
    REQUIRE(imported.changed && imported.conflicts.empty());
    REQUIRE(imported.records.size() == 1);
    REQUIRE(imported.records[0].id == "local-1");
    REQUIRE(imported.records[0].name == "www");
    REQUIRE(imported.records[0].content == "192.0.2.2");
    REQUIRE(imported.records[0].ttl == 600);
    REQUIRE(imported.remoteIdsByLocalId.at("local-1") == "remote-1");

    REQUIRE(plan("sync", {}).conflicts.size() == 1);
    const auto deleted = plan("sync_remote", {});
    REQUIRE(deleted.changed && deleted.records.empty());
    REQUIRE(deleted.remoteIdsByLocalId.empty());
    const auto retained = plan("sync_local", {});
    REQUIRE(retained.records.size() == 1);
    REQUIRE(retained.records[0].content == "192.0.2.1");

    // Identical snapshots produce no write plan, including provider FQDN conversion.
    auto identical = remote;
    identical.content = "192.0.2.1";
    identical.ttl = 300;
    const auto unchanged = plan("sync", {identical});
    REQUIRE(!unchanged.changed && unchanged.conflicts.empty());
    REQUIRE(unchanged.records.size() == 1);

    bool duplicateRejected = false;
    try { (void)plan("sync_remote", {remote, remote}); }
    catch (const std::runtime_error&) { duplicateRejected = true; }
    REQUIRE(duplicateRejected);
}
