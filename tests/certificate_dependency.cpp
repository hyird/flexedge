#include <ruvia/core/task.h>
#include <ruvia/core/task_scope.h>
#include <ruvia/web/app.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/write.hpp>

#include "service/config/rate_limit.h"
#include "service/features/certificate/queue.h"
#include "service/features/certificate/worker.h"
#include "service/features/dns_sync/zone_persistence.h"
#include "support/postgres_test.h"
#include "support/redis_test.h"

namespace {

namespace sync = service::sync_runtime;
namespace issuance = service::certificate_issuance;
using service::background::task_context;
constexpr std::string_view tenant{"00000000-0000-4000-8000-000000000b01"};
constexpr std::string_view zone{"00000000-0000-4000-8000-000000000b02"};
constexpr std::string_view provider{"00000000-0000-4000-8000-000000000b03"};
constexpr std::string_view dns_provider{"00000000-0000-4000-8000-000000000b04"};
constexpr std::string_view certificate{"00000000-0000-4000-8000-000000000b05"};
constexpr std::string_view challenge_id{"00000000-0000-4000-8000-000000000b06"};
constexpr std::string_view account_url{"https://acme-v02.api.letsencrypt.org/account/qa"};

void require(bool value, std::string_view message) {
    if (!value) {
        throw std::runtime_error("certificate dependency: " + std::string(message));
    }
}

// The production ACME client and JWS run unchanged. Its configured HTTP origin
// is a loopback fixture; captured requests prove that blocked work makes no
// account, order, authorization or challenge requests.
class acme_origin final {
public:
    acme_origin()
        : acceptor_(io_, {asio::ip::make_address("127.0.0.1"), 0}) {
        acceptor_.non_blocking(true);
        thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                asio::ip::tcp::socket socket(io_);
                asio::error_code error;
                acceptor_.accept(socket, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                if (error) {
                    break;
                }
                try {
                    reply(socket);
                } catch (...) {
                    std::lock_guard lock(mutex_);
                    failure_ = std::current_exception();
                }
            }
        });
    }

    ~acme_origin() {
        thread_.request_stop();
        release();
        thread_.join();
    }

    std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    std::size_t count(std::string_view target = {}) const {
        std::lock_guard lock(mutex_);
        if (failure_) {
            std::rethrow_exception(failure_);
        }
        return target.empty() ? targets_.size() : std::ranges::count(targets_, target);
    }

    void hold(std::string_view target) {
        std::lock_guard lock(mutex_);
        held_target_ = target;
        waiting_ = false;
    }

    bool waiting() const {
        std::lock_guard lock(mutex_);
        return waiting_;
    }

    void release() {
        std::lock_guard lock(mutex_);
        held_target_.clear();
        released_.notify_all();
    }

private:
    void reply(asio::ip::tcp::socket& socket) {
        asio::streambuf input;
        asio::read_until(socket, input, "\r\n\r\n");
        std::istream stream(&input);
        std::string method, target, protocol, line;
        stream >> method >> target >> protocol;
        std::getline(stream, line);
        std::size_t content_length = 0;
        while (std::getline(stream, line) && line != "\r") {
            if (line.starts_with("content-length:") || line.starts_with("Content-Length:")) {
                content_length = std::stoul(line.substr(line.find(':') + 1));
            }
        }
        if (input.size() < content_length) {
            asio::read(socket, input, asio::transfer_exactly(content_length - input.size()));
        }
        {
            std::unique_lock lock(mutex_);
            targets_.push_back(target);
            if (held_target_ == target) {
                waiting_ = true;
                released_.wait(lock, [&] { return held_target_.empty(); });
                waiting_ = false;
            }
        }
        std::string body, location;
        int status = 200;
        if (target == "/directory") {
            body = R"({"newNonce":"https://acme-v02.api.letsencrypt.org/nonce","newAccount":"https://acme-v02.api.letsencrypt.org/account","newOrder":"https://acme-v02.api.letsencrypt.org/orders"})";
        } else if (target == "/nonce") {
            body = "";
        } else if (target == "/orders") {
            status = 201;
            location = "Location: https://acme-v02.api.letsencrypt.org/order/fresh\r\n";
            body = R"({"status":"pending","authorizations":["https://acme-v02.api.letsencrypt.org/auth/fresh"],"finalize":"https://acme-v02.api.letsencrypt.org/finalize/fresh"})";
        } else if (target == "/auth/fresh") {
            body = R"({"status":"pending","identifier":{"type":"dns","value":"dependency.example.com"},"challenges":[{"type":"dns-01","url":"https://acme-v02.api.letsencrypt.org/challenge/fresh","token":"qa-token"}]})";
        } else if (target == "/challenge/fresh") {
            body = "{}";
        } else if (target == "/order/fresh") {
            body = R"({"status":"processing","finalize":"https://acme-v02.api.letsencrypt.org/finalize/fresh"})";
        } else {
            status = 500;
            body = R"({"detail":"unexpected ACME request"})";
        }
        const auto response = "HTTP/1.1 " + std::to_string(status) + " QA\r\nContent-Type: application/json\r\nReplay-Nonce: qa-nonce\r\n" +
                              location + "Connection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        asio::error_code error;
        asio::write(socket, asio::buffer(response), error);
        if (error && error != asio::error::broken_pipe && error != asio::error::connection_reset) {
            throw asio::system_error(error);
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    mutable std::mutex mutex_;
    std::condition_variable released_;
    std::string held_target_;
    bool waiting_{};
    std::vector<std::string> targets_;
    std::exception_ptr failure_;
    std::jthread thread_;
};

ruvia::task<void> prepare(ruvia::db_handle db) {
    std::string_view schema{R"sql(
CREATE TABLE sys_provider (id uuid PRIMARY KEY, tenant_id uuid NOT NULL, revision bigint DEFAULT 1,
    kind text, provider text, status text DEFAULT 'verified', config jsonb, runtime jsonb,
    deleted_at timestamptz, updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_dns_zone (id uuid PRIMARY KEY, tenant_id uuid NOT NULL, provider_id uuid,
    domain text, desired_revision bigint DEFAULT 10, synced_revision bigint DEFAULT 9,
    revision bigint DEFAULT 1, deleted_at timestamptz, sort bigint DEFAULT 1,
    config jsonb DEFAULT '{"records":[]}', runtime jsonb, sync_status text DEFAULT 'pending',
    last_synced_at timestamptz, last_error text, next_check_at timestamptz,
    updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_certificate (id uuid PRIMARY KEY, tenant_id uuid NOT NULL, provider_id uuid, dns_zone_id uuid,
    subject_alt_names text[] DEFAULT '{dependency.example.com}', issuance_revision bigint DEFAULT 1,
    issued_revision bigint DEFAULT 0, revision bigint DEFAULT 1, status text DEFAULT 'pending', expires_at timestamptz,
    config jsonb DEFAULT '{"auto_renew":false}', material jsonb DEFAULT '{}', issuance_state jsonb,
    deleted_at timestamptz, sort bigint DEFAULT 1, last_error text, updated_at timestamptz DEFAULT NOW());
CREATE TABLE sys_sync_task (
    id uuid PRIMARY KEY DEFAULT gen_random_uuid(), tenant_id uuid NOT NULL, resource_type text NOT NULL,
    provider_id uuid, dns_zone_id uuid, certificate_id uuid, website_id uuid,
    resource_id uuid GENERATED ALWAYS AS (COALESCE(provider_id, dns_zone_id, certificate_id, website_id)) STORED,
    operation text NOT NULL, version bigint NOT NULL, processed_version bigint NOT NULL DEFAULT 0,
    is_done boolean NOT NULL DEFAULT FALSE, is_ok boolean NOT NULL DEFAULT FALSE,
    error text NOT NULL DEFAULT '', count_fails integer NOT NULL DEFAULT 0,
    next_attempt_at timestamptz NOT NULL DEFAULT NOW(), lease_owner text, lease_until timestamptz,
    wait_task_id uuid, wait_version bigint, created_at timestamptz NOT NULL DEFAULT NOW(),
    updated_at timestamptz NOT NULL DEFAULT NOW(), UNIQUE (tenant_id, resource_type, resource_id),
    CHECK (NOT is_ok OR is_done), CHECK ((lease_owner IS NULL) = (lease_until IS NULL)),
    CHECK ((wait_task_id IS NULL) = (wait_version IS NULL)),
    CHECK (version > 0 AND processed_version >= 0 AND processed_version <= version));
CREATE TABLE sys_sync_event (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY, tenant_id uuid NOT NULL, task_id uuid NOT NULL,
    resource_type text NOT NULL, resource_id uuid NOT NULL, operation text NOT NULL, version bigint NOT NULL,
    outcome text NOT NULL, error text NOT NULL, attempt integer NOT NULL, started_at timestamptz,
    duration_ms bigint NOT NULL, detail jsonb NOT NULL, emitted_at timestamptz NOT NULL);
CREATE TABLE sys_sync_event_trace (event_id bigint PRIMARY KEY REFERENCES sys_sync_event(id), tenant_id uuid NOT NULL,
    summary jsonb NOT NULL, trace jsonb NOT NULL, trace_bytes integer NOT NULL);
CREATE TABLE sys_notification_outbox (id varchar(64) PRIMARY KEY, kind varchar(24) NOT NULL, payload text NOT NULL,
    created_at timestamptz NOT NULL DEFAULT NOW(), available_at timestamptz NOT NULL DEFAULT NOW(), attempts integer NOT NULL DEFAULT 0);
)sql"};
    while (!schema.empty()) {
        const auto end = schema.find(';');
        if (end == std::string_view::npos) {
            break;
        }
        (void)co_await db.execute(schema.substr(0, end));
        schema.remove_prefix(end + 1);
    }
    issuance::certificate_provider_runtime_data runtime;
    runtime.acme_account_ = issuance::certificate_provider_account_runtime_data{
        .private_key_envelope_ = service::utils::seal_secret(issuance::detail::private_key_pem(issuance::detail::generate_rsa_key().get())),
        .account_url_ = std::string(account_url)};
    const auto runtime_json = ruvia::to_json(issuance::to_output(runtime));
    (void)co_await db.execute("INSERT INTO sys_provider(id, tenant_id, kind, provider, config, runtime) VALUES ($1,$2,'certificate','letsencrypt','{\"credential_mode\":\"email\"}',$3::jsonb)",
        provider, tenant, std::string_view(runtime_json));
    (void)co_await db.execute("INSERT INTO sys_provider(id, tenant_id, kind, provider, config, runtime) VALUES ($1,$2,'dns','cloudflare','{}','{}')", dns_provider, tenant);
    (void)co_await db.execute("INSERT INTO sys_dns_zone(id, tenant_id, provider_id, domain) VALUES ($1,$2,$3,'example.com')", zone, tenant, dns_provider);
    (void)co_await db.execute("INSERT INTO sys_certificate(id, tenant_id, provider_id, dns_zone_id) VALUES ($1,$2,$3,$4)", certificate, tenant, provider, zone);
}

service::dns_sync::zone_runtime_data challenge_runtime(bool synced, std::int64_t revision = 10, bool unrelated_failure = false) {
    service::dns_sync::zone_runtime_data data;

    data.challenge_records_.push_back({.id_ = std::string(challenge_id), .name_ = "_acme-challenge.dependency.example.com", .content_ = "qa-challenge", .ttl_ = 600, .certificate_id_ = std::string(certificate)});
    data.record_states_.push_back({.id_ = std::string(challenge_id), .remote_record_id_ = "remote-qa", .sync_status_ = synced ? "synced" : "failed", .synced_revision_ = synced ? revision : revision - 1, .last_error_ = synced ? std::optional<std::string>{} : "record name conflict"});
    if (unrelated_failure) {
        data.record_states_.push_back({.id_ = "unrelated", .remote_record_id_ = {}, .sync_status_ = "failed", .synced_revision_ = 0, .last_error_ = "unrelated name conflict"});
    }
    return data;
}

issuance::validation_progress progress(std::int64_t revision = 1) {
    return issuance::validation_progress(
        {.revision_ = revision, .account_url_ = std::string(account_url),
            .order_url_ = "https://acme-v02.api.letsencrypt.org/order/fresh"},
        {{.authorization_url_ = "https://acme-v02.api.letsencrypt.org/auth/fresh",
            .challenge_url_ = "https://acme-v02.api.letsencrypt.org/challenge/fresh"}}, 10);
}

static_assert(!std::is_default_constructible_v<issuance::validation_progress>);
static_assert(!std::is_default_constructible_v<issuance::finalize_progress>);
static_assert(!std::is_constructible_v<issuance::issuance_progress, issuance::unpublished_order>);

void verify_progress_values(std::pmr::memory_resource* resource) {
    const auto validation = progress();
    const auto json = issuance::serialize_issuance_progress(validation, resource);
    const auto restored = issuance::parse_issuance_progress(json, 1, resource);
    require(restored && std::holds_alternative<issuance::validation_progress>(*restored),
        "published validation resumes as its own phase value");
    const auto& resumed = std::get<issuance::validation_progress>(*restored);
    require(resumed.dns_revision() == 10 && resumed.authorizations().size() == 1 &&
        resumed.authorizations().front().challenge_url_ == validation.authorizations().front().challenge_url_,
        "validation preserves the publication revision and challenge URLs");
    require(!issuance::parse_issuance_progress(json, 2, resource), "progress from another issuance revision is rejected");
    const auto parse = [&](std::string_view phase, std::optional<std::int64_t> dns_revision,
                           std::string_view key, std::string_view order_url = "order") {
        issuance::issuance_state_dto output({.resource_ = resource});
        output.set<"revision">(1);
        output.set<"phase">(phase);
        output.set<"account_url">(account_url);
        output.set<"order_url">(order_url);
        (void)output.ensure<"authorizations">();
        output.set<"private_key_envelope">(key);
        if (dns_revision) {
            output.set<"dns_revision">(*dns_revision);
        }
        const auto wire = ruvia::to_json(output, {.resource_ = resource});
        return issuance::parse_issuance_progress(std::string_view(wire), 1, resource);
    };
    require(!parse("validation", std::nullopt, "") && !parse("validation", 0, "") &&
        !parse("validation", -1, ""), "legacy or nonpositive DNS revisions cannot enter validation");
    require(!parse("finalize", 10, "") && !parse("unknown", 10, "key") &&
        !parse("validation", 10, "", ""), "missing keys, unknown phases and empty orders are rejected");
    const auto legacy_finalize = parse("finalize", std::nullopt, "stored-key");
    require(legacy_finalize && std::get<issuance::finalize_progress>(*legacy_finalize).private_key_envelope() == "stored-key",
        "legacy finalize progress remains resumable without a DNS revision");
    auto validated = progress();
    const issuance::finalize_progress finalized(std::move(validated), "stored-key");
    const auto final_json = issuance::serialize_issuance_progress(finalized, resource);
    const auto final_dto = ruvia::from_json<issuance::issuance_state_dto>(final_json, {.resource_ = resource});
    require(final_dto && final_dto->get<"phase">().view() == "finalize" &&
        final_dto->get<"authorizations">().empty() &&
        final_dto->get<"private_key_envelope">().view() == "stored-key" &&
        final_dto->get<"dns_revision">()->value_ == 10,
        "finalize serializes the unchanged JSON contract without validation-only fields");
    bool rejected_validation{}, rejected_finalize{};
    try {
        (void)issuance::validation_progress(validation.order(), {}, 0);
    } catch (const std::invalid_argument&) {
        rejected_validation = true;
    }
    try {
        (void)issuance::finalize_progress(validation.order(), "");
    } catch (const std::invalid_argument&) {
        rejected_finalize = true;
    }
    require(rejected_validation && rejected_finalize, "phase constructors reject missing publication or certificate keys");
    using disposition = issuance::failure_disposition;
    const auto check_failure = [](const std::exception_ptr& exception, disposition expected) {
        const auto failure = issuance::classify_issuance_failure(exception);
        require(failure.disposition_ == expected && !failure.message_.empty(),
            "each exception has one explicit issuance disposition and an error message");
    };
    check_failure(std::make_exception_ptr(issuance::acme_pending("pending")), disposition::resume_order);
    check_failure(std::make_exception_ptr(service::outbound_http::transport_error("network")), disposition::resume_order);
    check_failure(std::make_exception_ptr(issuance::acme_error("invalid order", false)), disposition::restart_order);
    check_failure(std::make_exception_ptr(issuance::acme_error("credentials", true)), disposition::terminal);
    check_failure(std::make_exception_ptr(std::invalid_argument("stored input")), disposition::terminal);
    check_failure(std::make_exception_ptr(std::runtime_error("storage")), disposition::restart_order);
    check_failure(std::make_exception_ptr(42), disposition::restart_order);
}

ruvia::task<void> reset(task_context& context, bool legacy = false) {
    auto db = context.db();
    (void)co_await db.execute("TRUNCATE sys_sync_event_trace, sys_sync_event, sys_sync_task, sys_notification_outbox");
    const auto runtime = ruvia::to_json(service::dns_sync::to_output(challenge_runtime(false)));
    (void)co_await db.execute("UPDATE sys_dns_zone SET desired_revision=10, synced_revision=9, runtime=$1::jsonb, sync_status='pending', last_error=NULL", std::string_view(runtime));
    // Deliberately malformed legacy wire data cannot be constructed as a
    // validation_progress: it has no proof of a published DNS revision.
    const auto state = legacy
        ? std::string(R"({"revision":1,"phase":"validation","account_url":"https://acme-v02.api.letsencrypt.org/account/qa","order_url":"https://acme-v02.api.letsencrypt.org/order/legacy","authorizations":[{"authorization_url":"https://acme-v02.api.letsencrypt.org/auth/legacy","challenge_url":"https://acme-v02.api.letsencrypt.org/challenge/legacy"}],"private_key_envelope":""})")
        : issuance::serialize_issuance_progress(progress(), context.pool());
    (void)co_await db.execute("UPDATE sys_certificate SET issuance_revision=1, issuance_state=$1::jsonb, status='issuing', last_error=NULL", std::string_view(state));
    auto transaction = co_await db.begin_transaction();
    service::background::queued_work work;
    (void)co_await service::dns_sync::enqueue_zone_revision(transaction, tenant, zone, 10, work);
    (void)co_await issuance::enqueue_certificate_revision(transaction, tenant, certificate, 1, sync::marker_operation::issue, work);
    co_await sync::commit_work(transaction, context, tenant, work);
}

ruvia::task<sync::claimed_marker> run_certificate(task_context& context, bool pending_deadline = false) {
    (void)co_await context.db().execute("UPDATE sys_sync_task SET next_attempt_at=NOW() WHERE resource_type='certificate' AND wait_task_id IS NULL");
    auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::certificate);
    require(marker.has_value(), "the production claim acquires the certificate");
    if (pending_deadline) {
        marker->claimed_at_ -= std::chrono::seconds(41);
    }
    co_await issuance::worker_detail::process_task(context, *marker);
    co_return std::move(*marker);
}

ruvia::task<void> finish_dns(task_context& context, bool success, bool challenge_synced = true, bool unrelated_failure = false) {
    (void)co_await context.db().execute("UPDATE sys_sync_task SET next_attempt_at=NOW() WHERE resource_type='dns_zone'");
    const auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    require(marker.has_value(), "the DNS owner acquires its real lease");
    const auto lease = sync::make_running_lease(marker->tenant_id_, marker->id_, marker->version_, context.lease_owner(), marker->claimed_at_, sync::marker_resource_type::dns_zone, zone);
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web", marker->claimed_at_);
    const service::dns_sync::detail::dns_task task{.id_ = marker->id_, .tenant_id_ = marker->tenant_id_, .resource_id_ = marker->resource_id_, .operation_ = marker->operation_, .version_ = marker->version_, .failures_ = marker->failures_, .claimed_at_ = marker->claimed_at_};
    if (success) {
        const service::dns_sync::zone_snapshot snapshot{
            .records_ = {}, .runtime_ = challenge_runtime(challenge_synced, marker->version_, unrelated_failure)};
        co_await service::dns_sync::detail::persist_synced_zone(context, trace, task, lease, snapshot, marker->version_, 1,
            challenge_synced && !unrelated_failure ? "" : "record conflict", sync::detail_none_json, true);
    } else {
        co_await service::dns_sync::detail::fail_dns_task(context, trace, task, "DNS credentials rejected", true, sync::detail_none_json);
    }
}

ruvia::task<void> require_waiting(task_context& context) {
    const auto rows = co_await context.db().query("SELECT wait_version=10 AND NOT is_done, issuance_state->>'dns_revision'='10' FROM sys_sync_task task JOIN sys_certificate certificate ON certificate.id=task.certificate_id WHERE task.resource_type='certificate'");
    require(rows.size() == 1 && rows.front()[0].as<bool>().value_or(false) && rows.front()[1].as<bool>().value_or(false), "waiting retains the durable DNS requirement and progress");
}

ruvia::task<void> require_terminal(task_context& context) {
    const auto rows = co_await context.db().query("SELECT task.is_done AND NOT task.is_ok, certificate.status='failed' AND certificate.issuance_state IS NULL, event.outcome='failed', task.error LIKE '%DNS-01%' FROM sys_sync_task task JOIN sys_certificate certificate ON certificate.id=task.certificate_id JOIN sys_sync_event event ON event.task_id=task.id WHERE task.resource_type='certificate' ORDER BY event.id DESC LIMIT 1");
    require(rows.size() == 1 && rows.front()[0].as<bool>().value() && rows.front()[1].as<bool>().value() && rows.front()[2].as<bool>().value() && rows.front()[3].as<bool>().value(), "unapplied terminal DNS yields an explicit terminal certificate result with no retry");
    require(!(co_await sync::claim_marker(context, sync::marker_resource_type::certificate)), "terminal certificate is not retried");
}

// Force DNS to commit while the certificate waits for the zone lock. The
// database confirms the blocked lock before the writer commits, so this checks
// the fresh read after the lock rather than depending on a scheduling delay.
ruvia::task<void> verify_result_during_gate(ruvia::web_worker_context& web, task_context& context,
    const acme_origin& origin, bool success) {
    co_await reset(context);
    const auto dns_marker = co_await sync::claim_marker(context, sync::marker_resource_type::dns_zone);
    ruvia::task_scope tasks(context.worker(), {.resource_ = context.pool()});
    task_context gated(web, tasks.get_stop_token());
    auto certificate_marker = co_await sync::claim_marker(gated, sync::marker_resource_type::certificate);
    require(dns_marker && certificate_marker, "both dependency owners have real leases");
    certificate_marker->claimed_at_ -= std::chrono::seconds(41);
    auto holder = co_await context.db().begin_transaction();
    (void)co_await holder.query("SELECT id FROM sys_dns_zone WHERE id=$1 FOR UPDATE", zone);
    const auto holder_identity = co_await holder.query("SELECT pg_backend_pid()");
    const auto holder_pid = holder_identity.front()[0].as<std::int64_t>().value();
    const auto before = origin.count();
    const auto challenges_before = origin.count("/challenge/fresh");
    std::exception_ptr failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await issuance::worker_detail::process_task(gated, *certificate_marker);
        } catch (...) {
            failure = std::current_exception();
        }
    };
    tasks.spawn(run());
    bool blocked = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto rows = co_await context.db().query(
            "SELECT COUNT(*) FROM pg_stat_activity WHERE datname=current_database() AND "
            "wait_event_type='Lock' AND $1=ANY(pg_blocking_pids(pid))",
            holder_pid);
        if (rows.front()[0].as<std::int64_t>().value() != 0) {
            blocked = true;
            break;
        }
        (void)co_await ruvia::sleep_for(context.worker(), std::chrono::milliseconds(10), context.get_stop_token());
    }
    if (!blocked) {
        co_await holder.rollback();
        tasks.request_stop();
        co_await tasks.join();
        if (failure) {
            std::rethrow_exception(failure);
        }
        require(false, "the certificate gate reached its zone lock");
    }
    const auto runtime = ruvia::to_json(service::dns_sync::to_output(challenge_runtime(success)));
    (void)co_await holder.execute("UPDATE sys_dns_zone SET synced_revision=$1,runtime=$2::jsonb,sync_status=$3 WHERE id=$4",
        std::int64_t{success ? 10 : 9}, std::string_view(runtime), success ? std::string_view{"synced"} : std::string_view{"failed"}, zone);
    const auto lease = sync::make_running_lease(tenant, dns_marker->id_, dns_marker->version_, context.lease_owner(),
        dns_marker->claimed_at_, sync::marker_resource_type::dns_zone, zone);
    sync::running_result_writer writer(holder, lease);
    const auto result = co_await writer.record(
        success ? sync::attempt_result::completed() : sync::attempt_result::failed("failed during gate"), sync::detail_none_json);
    require(result.kind() == sync::result_kind::recorded, "the dependency result owns its lease");
    service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::dns_sync, "web", dns_marker->claimed_at_);
    co_await writer.commit(context, trace);
    co_await tasks.join();
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (success) {
        require(origin.count("/challenge/fresh") == challenges_before + 1, "a result committed during the gate is observed and validates immediately");
    } else {
        co_await require_terminal(context);
        require(origin.count() == before, "a failure committed during the gate is terminal before any ACME request");
    }
    const auto waiter = co_await context.db().query("SELECT wait_task_id IS NULL FROM sys_sync_task WHERE resource_type='certificate'");
    require(waiter.front()[0].as<bool>().value(), "the certificate never parks after its dependency already completed");
}

ruvia::task<void> verify_running_lease(ruvia::web_worker_context& web, task_context& context,
    acme_origin& origin, bool lose_lease) {
    co_await reset(context, true);
    auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::certificate);
    require(marker.has_value(), "lease regression claims the certificate");
    origin.hold("/orders");
    ruvia::task_scope tasks(web.worker());
    task_context running(context, tasks.get_stop_token());
    bool completed{};
    std::exception_ptr run_failure, check_failure;
    auto run = [&]() -> ruvia::task<void> {
        try {
            co_await issuance::worker_detail::process_task(running, *marker);
        } catch (...) {
            run_failure = std::current_exception();
        }
        completed = true;
    };
    tasks.spawn(run());
    try {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!origin.waiting() && !completed && std::chrono::steady_clock::now() < deadline) {
            (void)co_await ruvia::sleep_for(web.worker(), std::chrono::milliseconds(10));
        }
        require(origin.waiting(), "the production ACME request is held in flight");
        const auto held = co_await context.db().query("SELECT lease_until::text FROM sys_sync_task WHERE id=$1", marker->id_);
        const std::string initial(held.front()[0].value().value());
        if (lose_lease) {
            (void)co_await context.db().execute("UPDATE sys_sync_task SET lease_owner='replacement' WHERE id=$1", marker->id_);
        }
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
        bool renewed{};
        while (!completed && !renewed && std::chrono::steady_clock::now() < deadline) {
            (void)co_await ruvia::sleep_for(web.worker(), std::chrono::milliseconds(100));
            if (!lose_lease) {
                const auto rows = co_await context.db().query(
                    "SELECT lease_until > $2::timestamptz + INTERVAL '10 seconds' FROM sys_sync_task WHERE id=$1",
                    marker->id_, initial);
                renewed = rows.front()[0].as<bool>().value();
            }
        }
        if (lose_lease) {
            require(completed, "losing the lease cancels a blocked ACME request before its response");
            const auto rows = co_await context.db().query(
                "SELECT lease_owner='replacement', (SELECT count(*) FROM sys_sync_event WHERE task_id=$1) FROM sys_sync_task WHERE id=$1",
                marker->id_);
            require(rows.front()[0].as<bool>().value() && rows.front()[1].as<int>() == 0,
                "a former owner cannot clear the new lease or persist a result");
        } else {
            require(renewed && !completed, "an in-flight ACME request renews its owning task lease");
            co_await sync::recover_stale_running(context, sync::marker_resource_type::certificate);
            const auto rows = co_await context.db().query("SELECT count(*) FROM sys_sync_event WHERE task_id=$1", marker->id_);
            require(rows.front()[0].as<int>() == 0, "a live renewed attempt cannot be recovered by another claimer");
        }
    } catch (...) {
        check_failure = std::current_exception();
    }
    origin.release();
    if (check_failure) {
        tasks.request_stop();
    }
    co_await tasks.join();
    if (check_failure) {
        std::rethrow_exception(check_failure);
    }
    if (run_failure) {
        std::rethrow_exception(run_failure);
    }
}

ruvia::task<void> verify_failure_dispositions(task_context& context) {
    using disposition = issuance::failure_disposition;
    for (const auto decision : {disposition::resume_order, disposition::restart_order, disposition::terminal}) {
        co_await reset(context);
        const auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::certificate);
        require(marker.has_value(), "disposition regression acquires the certificate lease");
        const issuance::worker_detail::certificate_task task{.id_ = marker->id_, .tenant_id_ = marker->tenant_id_,
            .certificate_id_ = marker->resource_id_, .version_ = marker->version_, .failures_ = marker->failures_,
            .operation_ = std::string(sync::marker_operation_name(marker->operation_)), .claimed_at_ = marker->claimed_at_};
        service::attempt_trace::recorder trace(service::attempt_trace::trace_kind::certificate, "web", marker->claimed_at_);
        const issuance::certificate_attempt attempt{.operation_ = task.operation_, .provider_ = "letsencrypt",
            .domains_ = {"dependency.example.com"}, .stage_ = issuance::certificate_stage::validation, .issued_ = std::nullopt};
        co_await issuance::worker_detail::fail_certificate_task(context, trace, task, "DNS-01 disposition",
            decision, attempt.to_json(context.pool()));
        const auto rows = co_await context.db().query(
            "SELECT certificate.issuance_state IS NOT NULL, task.is_done, task.is_ok, zone.runtime::text "
            "FROM sys_certificate certificate JOIN sys_sync_task task ON task.certificate_id=certificate.id "
            "JOIN sys_dns_zone zone ON zone.id=certificate.dns_zone_id WHERE task.resource_type='certificate'");
        const auto runtime = service::dns_sync::parse_stored_runtime(rows.front()[3].value().value_or(""));
        require(runtime && rows.front()[0].as<bool>().value() == (decision == disposition::resume_order) &&
            runtime->challenge_records_.empty() == (decision != disposition::resume_order),
            "only resume_order preserves durable progress and published challenge records");
        require(rows.front()[1].as<bool>().value() == (decision == disposition::terminal) &&
            !rows.front()[2].as<bool>().value(), "only terminal completes the failed task");
        if (decision == disposition::terminal) {
            co_await require_terminal(context);
        }
    }
}

ruvia::task<void> verify_finalize_recovery(ruvia::web_worker_context& web, task_context& context,
    const acme_origin& origin) {
    co_await reset(context);
    const auto marker = co_await sync::claim_marker(context, sync::marker_resource_type::certificate);
    require(marker.has_value(), "finalize recovery acquires the certificate lease");
    const issuance::worker_detail::certificate_task task{.id_ = marker->id_, .tenant_id_ = marker->tenant_id_,
        .certificate_id_ = marker->resource_id_, .version_ = marker->version_, .failures_ = marker->failures_,
        .operation_ = std::string(sync::marker_operation_name(marker->operation_)), .claimed_at_ = marker->claimed_at_};
    const auto lease = sync::make_running_lease(tenant, marker->id_, marker->version_, context.lease_owner(),
        marker->claimed_at_, sync::marker_resource_type::certificate, certificate);
    const auto work = co_await issuance::worker_detail::load_work(context, task);
    const auto key = issuance::detail::private_key_pem(issuance::detail::generate_rsa_key().get());
    const auto envelope = service::utils::seal_secret(key);
    const issuance::finalize_progress finalized(progress().order(), envelope, 10);
    require(co_await issuance::worker_detail::enter_finalize(context, task, lease, work, finalized),
        "finalize stores the certificate key before any finalization request");
    // Simulate a process exiting after that commit. Lease recovery must not
    // discard or regenerate the certificate key of this already-valid order.
    (void)co_await context.db().execute("UPDATE sys_sync_task SET lease_until=NOW()-INTERVAL '1 second' WHERE id=$1", marker->id_);
    co_await sync::recover_stale_running(context, sync::marker_resource_type::certificate);
    const auto before_orders = origin.count("/orders");
    const auto before_authorizations = origin.count("/auth/fresh");
    const auto before_challenges = origin.count("/challenge/fresh");
    const auto before_finalize = origin.count("/order/fresh");
    task_context restarted(web);
    (void)co_await run_certificate(restarted, true);
    const auto rows = co_await context.db().query("SELECT issuance_state::text FROM sys_certificate WHERE id=$1", certificate);
    const auto restored = issuance::parse_issuance_progress(rows.front()[0].value().value_or(""), 1, context.pool());
    require(restored && std::holds_alternative<issuance::finalize_progress>(*restored),
        "pending finalization resumes and keeps its distinct durable phase");
    const auto& final_state = std::get<issuance::finalize_progress>(*restored);
    require(final_state.private_key_envelope() == envelope &&
        service::utils::open_secret(final_state.private_key_envelope()) == key,
        "restart and pending finalization preserve the original certificate private key");
    require(origin.count("/orders") == before_orders && origin.count("/auth/fresh") == before_authorizations &&
        origin.count("/challenge/fresh") == before_challenges && origin.count("/order/fresh") > before_finalize,
        "finalize recovery bypasses DNS validation and resumes the same order");
}

ruvia::task<void> verify(ruvia::web_worker_context& web, acme_origin& origin) {
    task_context context(web);
    co_await prepare(context.db());
    verify_progress_values(context.pool());
    co_await reset(context);
    auto before = origin.count();
    (void)co_await run_certificate(context);
    co_await require_waiting(context);
    require(origin.count() == before, "pending DNS invokes no ACME endpoint");
    co_await finish_dns(context, false);
    const auto resumed = co_await context.db().query("SELECT wait_task_id IS NULL FROM sys_sync_task WHERE resource_type='certificate'");
    require(resumed.front()[0].as<bool>().value(), "terminal DNS releases the temporary wait reference");
    const auto old_certificate = co_await run_certificate(context);
    co_await require_terminal(context);
    require(origin.count() == before, "terminal DNS invokes no ACME endpoint after dependency wakeup");

    // A manual resend resets the same marker and advances the certificate
    // revision. It creates a fresh order and persists its actual DNS revision.
    auto transaction = co_await context.db().begin_transaction();
    (void)co_await transaction.execute("UPDATE sys_certificate SET issuance_revision=2, status='pending', last_error=NULL WHERE id=$1", certificate);
    service::background::queued_work work;
    const auto reset_id = co_await issuance::enqueue_certificate_revision(transaction, tenant, certificate, 2, sync::marker_operation::issue, work);
    co_await sync::commit_work(transaction, context, tenant, work);
    require(reset_id == old_certificate.id_, "manual resend retains the task identity");
    (void)co_await run_certificate(context);
    const auto published = co_await context.db().query("SELECT (certificate.issuance_state->>'dns_revision')::bigint=zone.desired_revision AND task.wait_version=zone.desired_revision, certificate.issuance_state->>'revision'='2' FROM sys_certificate certificate JOIN sys_dns_zone zone ON zone.id=certificate.dns_zone_id JOIN sys_sync_task task ON task.certificate_id=certificate.id");
    require(published.front()[0].as<bool>().value() && published.front()[1].as<bool>().value(), "fresh challenge publication and its real required revision commit together");
    require(origin.count("/orders") == 1 && origin.count("/challenge/fresh") == 0, "manual resend creates an order and waits before answering challenges");

    // The same revision can be requeued after terminal failure, replacing its
    // current outcome. The persisted certificate requirement survives a fresh
    // context and waits until the retried DNS revision is actually applied.
    co_await reset(context);
    (void)co_await run_certificate(context);
    co_await finish_dns(context, false);
    auto retry = co_await context.db().begin_transaction();
    service::background::queued_work retry_work;
    (void)co_await service::dns_sync::enqueue_zone_revision(retry, tenant, zone, 10, retry_work);
    co_await sync::commit_work(retry, context, tenant, retry_work);
    before = origin.count();
    task_context restarted(web);
    (void)co_await run_certificate(restarted);
    co_await require_waiting(restarted);
    require(origin.count() == before, "same-revision manual DNS retry and restart remain blocked before ACME");
    co_await finish_dns(restarted, true, true, true);
    (void)co_await run_certificate(restarted, true);
    require(origin.count("/challenge/fresh") == 1, "the applied challenge resumes even when an unrelated DNS record failed");
    const auto kept = co_await context.db().query("SELECT issuance_state->>'dns_revision'='10' FROM sys_certificate");
    require(kept.front()[0].as<bool>().value(), "controlled pending ACME preserves the order for the next attempt");

    // DNS records can partially fail while the zone revision and task still
    // complete. The certificate must inspect its own record results.
    co_await reset(context);
    (void)co_await run_certificate(context);
    co_await finish_dns(context, true, false);
    before = origin.count();
    (void)co_await run_certificate(context);
    co_await require_terminal(context);
    require(origin.count() == before, "a failed challenge never reaches ACME even with a completed zone revision");

    co_await reset(context);
    // A nominally successful marker without actual publication is also unsafe.
    (void)co_await context.db().execute("UPDATE sys_sync_task SET is_done=TRUE,is_ok=TRUE,processed_version=version WHERE resource_type='dns_zone'");
    before = origin.count();
    (void)co_await run_certificate(context);
    co_await require_terminal(context);
    require(origin.count() == before, "marker success alone cannot replace the actual required DNS revision");

    co_await reset(context);
    const auto synced_runtime = ruvia::to_json(service::dns_sync::to_output(challenge_runtime(true)));
    (void)co_await context.db().execute("UPDATE sys_dns_zone SET synced_revision=10,runtime=$1::jsonb,desired_revision=11", std::string_view(synced_runtime));
    (void)co_await context.db().execute("UPDATE sys_sync_task SET version=11,is_done=TRUE,is_ok=FALSE,processed_version=11 WHERE resource_type='dns_zone'");
    const auto challenges_before = origin.count("/challenge/fresh");
    (void)co_await run_certificate(context, true);
    require(origin.count("/challenge/fresh") == challenges_before + 1, "a later failed DNS task does not invalidate an already applied required revision");

    co_await reset(context);
    (void)co_await context.db().execute("DELETE FROM sys_sync_task WHERE resource_type='dns_zone'");
    before = origin.count();
    (void)co_await run_certificate(context);
    co_await require_terminal(context);
    require(origin.count() == before, "a missing DNS marker maps LEFT JOIN nulls and fails before any ACME request");

    co_await verify_result_during_gate(web, context, origin, false);
    co_await verify_result_during_gate(web, context, origin, true);

    co_await reset(context, true);
    const auto orders_before = origin.count("/orders");
    (void)co_await run_certificate(context);
    require(origin.count("/orders") == orders_before + 1 && origin.count("/auth/legacy") == 0 && origin.count("/challenge/legacy") == 0,
        "legacy progress lacking a required revision rebuilds its order without validating stale challenges");
    const auto legacy = co_await context.db().query("SELECT (certificate.issuance_state->>'dns_revision')::bigint=zone.desired_revision AND task.wait_version=zone.desired_revision FROM sys_certificate certificate JOIN sys_dns_zone zone ON zone.id=certificate.dns_zone_id JOIN sys_sync_task task ON task.certificate_id=certificate.id");
    require(legacy.front()[0].as<bool>().value(), "legacy replacement persists a fresh DNS dependency");
    co_await verify_running_lease(web, context, origin, false);
    co_await verify_running_lease(web, context, origin, true);
    co_await verify_failure_dispositions(context);
    co_await verify_finalize_recovery(web, context, origin);
}

}  // namespace

int main() {
    try {
        const auto config = test_support::isolated_postgres_config();
        const auto redis = test_support::isolated_redis_config();
        if (!config || !redis) {
            return 77;
        }
        const test_support::postgres_test_database database(*config);
        acme_origin origin;
        asio::io_context io;
        asio::ip::tcp::acceptor listener(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto port = listener.local_endpoint().port();
        listener.close();
        service::config::configure_outbound_origins(service::config::make_outbound_origins(""));
        service::utils::configure_secret_key("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
        service::config::configure_rate_limits({.outbound_global_ = {10000, std::chrono::seconds(1)}, .outbound_account_ = {10000, std::chrono::seconds(1)}});
        auto& app = ruvia::app();
        auto worker_database = database.config();
        worker_database.pool_size_per_worker_ = 3;
        app.database({.config_ = worker_database});
        app.redis({.config_ = *redis});
        app.get_http_client({.alias_ = "lets-encrypt", .config_ = {.scheme_ = ruvia::http_scheme::http, .host_ = "127.0.0.1", .port_ = origin.port(), .connection_count_ = 1}});
        app.server({.worker_count_ = 1});
        app.listen({.address_ = "127.0.0.1", .http_ = port});
        app.use_worker_state<service::background::work_available::claim_signals>();
        std::exception_ptr failure;
        bool completed{};
        app.on_start([&] {
            for (const auto& worker : app.workers()) {
                require(worker.post([&](ruvia::web_worker_context& web) -> ruvia::task<void> {
                                  try {
                                      co_await verify(web, origin);
                                      completed = true;
                                  } catch (...) {
                                      failure = std::current_exception();
                                  }
                                  app.stop();
                              })
                            .accepted(),
                    "the verification task is accepted");
            }
        });
        app.run();
        if (failure) {
            std::rethrow_exception(failure);
        }
        require(completed, "the real worker finishes before shutdown");
        std::cout << "Certificate DNS dependency: real PostgreSQL result/wakeup/retry, actual challenge publication, terminal and partial failures, revision readiness, restart, manual resend and legacy replacement passed; ACME is a controlled loopback fixture\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
