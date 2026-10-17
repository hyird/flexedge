#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "service/features/website_dns/maintenance.h"
#include "service/features/website_dns/persistence.h"
#include "support/postgres_test.h"
#include "support/recorded_notifications.h"

namespace {
constexpr std::string_view tenant{"02000000-0000-0000-0000-000000000001"};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ruvia::task<void> prepare(ruvia::db_handle db) {
    std::string_view schema{R"sql(
CREATE TABLE sys_website (
 id uuid PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id uuid NOT NULL, revision bigint DEFAULT 1,
 runtime jsonb DEFAULT '{}', deleted_at timestamptz);
CREATE TABLE sys_sync_task (
 id uuid PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id uuid NOT NULL, resource_type text NOT NULL,
 website_id uuid, resource_id uuid GENERATED ALWAYS AS (website_id) STORED,
 operation text DEFAULT 'apply', version bigint DEFAULT 1, processed_version bigint DEFAULT 1,
 is_done boolean DEFAULT true, is_ok boolean DEFAULT true, error text DEFAULT '', count_fails integer DEFAULT 0,
 next_attempt_at timestamptz DEFAULT NOW(), lease_owner text, lease_until timestamptz,
 wait_task_id uuid, wait_version bigint, created_at timestamptz DEFAULT NOW(), updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_sync_event (
 id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, tenant_id uuid, task_id uuid, resource_type text,
 resource_id uuid, operation text, version bigint, outcome text, error text, attempt integer,
 started_at timestamptz, duration_ms bigint, detail jsonb, emitted_at timestamptz);
CREATE TABLE sys_sync_event_trace (
 event_id bigint PRIMARY KEY REFERENCES sys_sync_event(id), tenant_id uuid,
 summary jsonb, trace jsonb, trace_bytes integer);
CREATE TABLE sys_notification_outbox (
 id varchar(64) PRIMARY KEY, kind varchar(24), payload text, created_at timestamptz DEFAULT NOW(),
 available_at timestamptz DEFAULT NOW(), attempts integer DEFAULT 0);
)sql"};
    while (!schema.empty()) {
        const auto end = schema.find(';');
        if (end == std::string_view::npos) {
            break;
        }
        (void)co_await db.execute(schema.substr(0, end));
        schema.remove_prefix(end + 1);
    }
    (void)co_await db.execute("INSERT INTO sys_website(tenant_id) SELECT $1::uuid FROM generate_series(1,150)", tenant);
    (void)co_await db.execute(
        "INSERT INTO sys_sync_task(tenant_id, resource_type, website_id, next_attempt_at) "
        "SELECT tenant_id, 'website', id, NOW() - row_number() OVER () * INTERVAL '1 minute' FROM sys_website");
}

service::website_dns::website_runtime_data runtime(std::string status, std::string error = {}) {
    return {.domain_states_ = {{.id_ = "external-domain", .resolution_status_ = std::move(status), .last_verified_at_ = "2026-10-18T00:00:00Z", .last_error_ = std::move(error)}}};
}

service::website_dns::website_domain_attempt attempt(const service::website_dns::website_runtime_data& output) {
    service::website_dns::website_domain_attempt result;
    result.start("edge.example.com", output.domain_states_.size());
    for (const auto& state : output.domain_states_) {
        result.record_check({.hostname_ = "external.example.org", .query_name_ = "external.example.org", .resolved_ = {}, .status_ = *state.resolution_status_, .error_ = state.last_error_.value_or("")});
    }
    return result;
}

template <typename context_type>
ruvia::task<void> persist(context_type& context, const service::sync_runtime::claimed_marker& task,
    const service::website_dns::website_runtime_data& output, bool inspection = true) {
    const auto lease = service::sync_runtime::make_running_lease(task.tenant_id_, task.id_, task.version_,
        context.lease_owner(), task.claimed_at_, service::sync_runtime::marker_resource_type::website, task.resource_id_);
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::website_domains, "web", task.claimed_at_);
    co_await service::website_dns::persist_website_check(context, trace, lease, output, attempt(output), inspection);
}

ruvia::task<void> verify(ruvia::db_client& client, ruvia::db_client& peer) {
    namespace dns = service::website_dns;
    co_await client.connect();
    co_await peer.connect();
    auto db = client.handle();
    co_await prepare(db);
    test_support::recorded_notifications first{.handle_ = db, .lease_owner_ = "first"};
    test_support::recorded_notifications second{.handle_ = peer.handle(), .lease_owner_ = "second"};
    const auto a = co_await dns::detail::claim_due_check(first);
    const auto b = co_await dns::detail::claim_due_check(second);
    require(a && b && a->id_ != b->id_, "concurrent inspections have distinct leases");
    const auto stable = runtime("verified");
    const auto stable_json = ruvia::to_json(dns::to_output(stable));
    (void)co_await db.execute("UPDATE sys_website SET runtime = $2::jsonb WHERE id = $1", a->resource_id_, std::string_view(stable_json));
    co_await persist(first, *a, stable);
    const auto quiet = co_await db.query(
        "SELECT is_done AND is_ok AND lease_until IS NULL AND next_attempt_at > NOW() + INTERVAL '9 minutes', "
        "(SELECT count(*) FROM sys_sync_event) FROM sys_sync_task WHERE id = $1",
        a->id_);
    require(quiet.front()[0].as<bool>().value_or(false) && quiet.front()[1].as<int>().value_or(-1) == 0 && first.sent_.empty(),
        "unchanged inspection advances schedule without history or notification");
    const auto invalid = runtime("invalid", "期望 edge.example.com，实际 wrong.example.net");
    co_await persist(second, *b, invalid);
    const auto failed = co_await db.query(
        "SELECT is_done AND NOT is_ok AND count_fails = 1 AND next_attempt_at > NOW() + INTERVAL '9 minutes', "
        "error, (SELECT count(*) FROM sys_sync_event) FROM sys_sync_task WHERE id = $1",
        b->id_);
    require(failed.front()[0].as<bool>().value_or(false) && failed.front()[1].value().value_or("").find("external.example.org") != std::string_view::npos &&
                failed.front()[2].as<int>().value_or(0) == 1 && second.sent_.size() == 1 && second.committed_.back(),
        "external DNS mismatch becomes a committed failed task and notification with hostname and reason");
    std::unordered_set<std::string> checked{a->id_, b->id_};
    for (int index = 0; index < 148; ++index) {
        const auto next = co_await dns::detail::claim_due_check(first);
        require(next && checked.emplace(next->id_).second, "all websites beyond a bounded batch are inspected once");
    }
    require(!co_await dns::detail::claim_due_check(second), "leased and future inspections cannot be claimed again");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 minute' WHERE id = $1", b->id_);
    const auto repeated = co_await dns::detail::claim_due_check(second);
    require(repeated && repeated->id_ == b->id_, "a failed settled task is periodically checked again");
    co_await persist(second, *repeated, invalid);
    require(second.sent_.size() == 1, "the same external mismatch does not notify again");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 minute' WHERE id = $1", b->id_);
    const auto recovered = co_await dns::detail::claim_due_check(second);
    require(recovered.has_value(), "recovery inspection is due");
    co_await persist(second, *recovered, stable);
    const auto recovery = co_await db.query(
        "SELECT is_done AND is_ok AND error = '' AND count_fails = 0, "
        "(SELECT count(*) FROM sys_sync_event) FROM sys_sync_task WHERE id = $1",
        b->id_);
    require(recovery.front()[0].as<bool>().value_or(false) && recovery.front()[1].as<int>().value_or(0) == 2 && second.sent_.size() == 2,
        "corrected DNS clears the warning and publishes one recovery");
    (void)co_await db.execute("UPDATE sys_sync_task SET next_attempt_at = NOW() - INTERVAL '1 minute' WHERE id = $1", a->id_);
    const auto stale = co_await dns::detail::claim_due_check(first);
    require(stale.has_value(), "stale result fixture is claimable");
    (void)co_await db.execute("UPDATE sys_website SET revision = 2 WHERE id = $1", a->resource_id_);
    co_await persist(first, *stale, invalid);
    const auto protected_state = co_await db.query(
        "SELECT runtime = $2::jsonb FROM sys_website WHERE id = $1", a->resource_id_, std::string_view(stable_json));
    require(protected_state.front()[0].as<bool>().value_or(false) && first.sent_.empty(),
        "an inspection of an old revision cannot overwrite runtime or notify");
    (void)co_await db.execute("UPDATE sys_sync_task SET is_done = false, next_attempt_at = NOW() WHERE id = $1", b->id_);
    require(!co_await dns::detail::claim_due_check(first), "periodic inspection never takes over pending work or obsolete revisions");
}
}  // namespace

int main() try {
    const auto base = test_support::isolated_postgres_config();
    if (!base) {
        return 77;
    }
    const test_support::postgres_test_database database(*base);
    return database.run(verify);
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
