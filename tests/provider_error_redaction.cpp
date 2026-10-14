#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/features/attempt_trace/recorder.h"
#include "service/features/attempt_trace/redaction.h"
#include "service/features/certificate/acme.h"
#include "service/features/dns/aliyun.h"
#include "service/features/dns/cloudflare.h"
#include "service/features/dns_sync/failure.h"
#include "service/features/logging/logger.h"
#include "service/features/provider_verification/failure.h"
#include "service/features/sync_runtime/error.h"
#include "support/attempt_trace_fixture.h"

// Provider error replies that echo credentials (Aliyun's string to sign with
// the AccessKeyId, a token in a header-like string, EAB material in an ACME
// problem) must leave the attempt redacted: in the driver's error text, in
// the stored task error and in the server log line.
namespace {

using service::attempt_trace::recorder;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("provider error redaction: " + std::string(message));
    }
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::vector<std::string> canaries;

std::string secret(std::string_view label) {
    canaries.push_back(test_support::attempt_trace::canary(label));
    return canaries.back();
}

recorder new_recorder() {
    return recorder(service::attempt_trace::trace_kind::dns_sync, "dns-sync", std::chrono::steady_clock::now());
}

class capturing_buffer final : public std::streambuf {
public:
    [[nodiscard]] std::string content() const {
        std::lock_guard lock(mutex_);
        return content_;
    }

protected:
    std::streamsize xsputn(const char* bytes, std::streamsize size) override {
        std::lock_guard lock(mutex_);
        content_.append(bytes, static_cast<std::size_t>(size));
        return size;
    }

    int overflow(int value) override {
        if (value == traits_type::eof()) {
            return traits_type::not_eof(value);
        }
        std::lock_guard lock(mutex_);
        content_.push_back(static_cast<char>(value));
        return value;
    }

private:
    mutable std::mutex mutex_;
    std::string content_;
};

std::string aliyun_signature_mismatch(std::string_view access_key_id, std::string_view nonce, std::string_view tail) {
    return R"({"RequestId":"6C1F-REQ","HostId":"alidns.aliyuncs.com","Code":"SignatureDoesNotMatch",)"
           R"("Message":"Specified signature is not matched with our calculation. server string to sign is:GET&%2F&AccessKeyId%3D)" +
           std::string(access_key_id) +
           "%26Action%3DDescribeDomains%26Format%3DJSON%26SignatureMethod%3DHMAC-SHA1%26SignatureNonce%3D" +
           std::string(nonce) + "%26SignatureVersion%3D1.0%26Timestamp%3D2026-10-14T10%253A00%253A00Z%26Version%3D2015-01-09" +
           std::string(tail) + R"(","Recommend":"https://api.aliyun.com/troubleshoot?q=SignatureDoesNotMatch"})";
}

// The reply as the driver turns it into an error, then the stored task
// error (DNS sync and provider verification classify Aliyun errors) and the
// log line written for it.
void verify_aliyun_signature_mismatch() {
    const auto access_key_id = secret("akid");
    const auto access_key_secret = secret("aksecret");
    const auto nonce = secret("nonce");
    // A reply that also names the secret in free text: only the attempt's
    // known credentials can catch that.
    const auto body = aliyun_signature_mismatch(access_key_id, nonce, " (request signed by " + access_key_secret + ")");
    std::pmr::monotonic_buffer_resource pool;
    auto trace = new_recorder();
    // As aliyun_client::send does before every request.
    trace.register_secret(access_key_id);
    trace.register_secret(access_key_secret);
    const auto error = service::dns::detail::aliyun_reply_error(trace, body, service::dns::aliyun_error_code::upstream_failed,
        "阿里云 DNS 域名查询失败", &pool);
    const std::string_view text = error.what();
    require(error.code() == service::dns::aliyun_error_code::credential_invalid, "SignatureDoesNotMatch is a credential error");
    require(text.starts_with("SignatureDoesNotMatch: Specified signature is not matched with our calculation."),
        "code and message stay readable: " + std::string(text));
    require(contains(text, "server string to sign is:[REDACTED]"), "string to sign dropped: " + std::string(text));
    test_support::attempt_trace::require_no_canary(text, canaries, "aliyun error");

    const auto dns_failure = service::dns_sync::detail::classify_task_failure(std::make_exception_ptr(error));
    require(dns_failure.permanent_, "credential errors are permanent");
    const auto dns_stored = service::sync_runtime::attempt_error(trace, dns_failure.message_);
    require(dns_stored == text, "stored DNS task error is the redacted text");
    test_support::attempt_trace::require_no_canary(dns_stored, canaries, "DNS task error");
    service::sync_runtime::log_attempt_failure("DNS sync task", "aliyun-dns-task", dns_stored);

    const auto provider_failure =
        service::provider_verification::detail::classify_task_failure(std::make_exception_ptr(error));
    const auto provider_stored = service::sync_runtime::attempt_error(trace, provider_failure.message_);
    test_support::attempt_trace::require_no_canary(provider_stored, canaries, "provider task error");
    service::sync_runtime::log_attempt_failure("Provider verification marker", "aliyun-provider-task", provider_stored);

    // Without any registered credential the patterns alone drop the echo.
    const auto unregistered_id = secret("akidplain");
    const auto unregistered_nonce = secret("nonceplain");
    auto bare = new_recorder();
    const auto bare_error = service::dns::detail::aliyun_reply_error(bare,
        aliyun_signature_mismatch(unregistered_id, unregistered_nonce, ""), service::dns::aliyun_error_code::upstream_failed,
        "阿里云 DNS 域名查询失败", &pool);
    test_support::attempt_trace::require_no_canary(bare_error.what(), canaries, "aliyun error without credentials");
}

void verify_cloudflare_token_echo() {
    const auto token = secret("cftoken");
    const auto account = secret("cfaccount");
    const auto body =
        R"({"success":false,"errors":[{"code":6111,"message":"Invalid format for Authorization header: Authorization: Bearer )" +
        token + "; X-Auth-Key: " + token + "; account " + account + R"("}],"messages":[],"result":null})";
    std::pmr::monotonic_buffer_resource pool;
    const auto parsed = ruvia::from_json<service::dns::cloudflare_error_envelope>(body, {.resource_ = &pool});
    require(parsed.has_value(), "cloudflare error envelope parses");
    auto trace = new_recorder();
    // As cloudflare_client::send and find_zone do.
    trace.register_secret(token);
    trace.register_secret(account);
    const auto error = service::dns::detail::cloudflare_reply_error(trace, parsed,
        service::dns::cloudflare_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
    const std::string_view text = error.what();
    require(text.starts_with("Invalid format for Authorization header: Authorization: Bearer [REDACTED]"),
        "message stays readable: " + std::string(text));
    test_support::attempt_trace::require_no_canary(text, canaries, "cloudflare error");

    const auto failure = service::dns_sync::detail::classify_task_failure(std::make_exception_ptr(error));
    const auto stored = service::sync_runtime::attempt_error(trace, failure.message_);
    test_support::attempt_trace::require_no_canary(stored, canaries, "cloudflare task error");
    service::sync_runtime::log_attempt_failure("DNS sync task", "cloudflare-dns-task", stored);

    // The bearer pattern alone hides an unregistered token.
    const auto unregistered = secret("cfplain");
    const auto bare_body = R"({"success":false,"errors":[{"code":10000,"message":"Authentication error: Authorization: Bearer )" +
                           unregistered + R"("}]})";
    const auto bare_parsed = ruvia::from_json<service::dns::cloudflare_error_envelope>(bare_body, {.resource_ = &pool});
    auto bare = new_recorder();
    const auto bare_error = service::dns::detail::cloudflare_reply_error(bare, bare_parsed,
        service::dns::cloudflare_error_code::dns_failed, "Cloudflare DNS 记录保存失败");
    require(bare_error.code() == service::dns::cloudflare_error_code::authorization_failed, "code 10000 is an authorization error");
    test_support::attempt_trace::require_no_canary(bare_error.what(), canaries, "cloudflare error without credentials");
}

void verify_acme_problem_echo() {
    const auto key_id = secret("kid");
    const auto hmac_key = secret("hmac");
    const auto email = secret("mail") + "@example.com";
    const auto body = R"({"type":"urn:ietf:params:acme:error:unauthorized","detail":"External account binding for )" + key_id +
                      " signed with " + hmac_key + " is invalid; contact mailto:" + email + R"(","status":401})";
    std::pmr::monotonic_buffer_resource pool;
    auto trace = recorder(service::attempt_trace::trace_kind::certificate, "certificate", std::chrono::steady_clock::now());
    // As acme_client::register_account does.
    trace.register_secret(email);
    trace.register_secret(key_id);
    trace.register_secret(hmac_key);
    const auto error = service::certificate_issuance::detail::acme_problem_error(trace, body,
        ruvia::http_status_code::from_value(401), "ACME 请求失败", &pool);
    const std::string_view text = error.what();
    require(error.permanent(), "unauthorized problems are permanent");
    require(text.starts_with("ACME 请求失败：External account binding for [REDACTED]"), "detail stays readable: " + std::string(text));
    test_support::attempt_trace::require_no_canary(text, canaries, "acme error");
    const auto stored = service::sync_runtime::attempt_error(trace, text);
    service::sync_runtime::log_attempt_failure("Certificate sync marker", "acme-task", stored);
}

// Credentials are remembered even after the values observed in traffic
// (signatures, nonces) filled the redactor's cap.
void verify_credentials_beyond_observed_cap() {
    test_support::attempt_trace::ensure_outbound_origins();
    auto trace = new_recorder();
    const auto observe = [&trace](std::string target) {
        test_support::attempt_trace::perform(trace,
            {.call_ = {.origin_alias_ = service::config::aliyun_dns_origin_alias,
                 .operation_ = service::attempt_trace::outbound_operation::aliyun_describe_domains},
                .target_ = std::move(target),
                .response_headers_ = {{"content-type", "application/json"}},
                .response_body_ = "{}"});
    };
    for (int index = 0; index < 60; ++index) {
        std::string target = "/?Action=DescribeDomains";
        for (int parameter = 0; parameter < 10; ++parameter) {
            target += "&Signature" + std::to_string(parameter) + "=observed" + std::to_string(index) + "x" +
                      std::to_string(parameter) + "padding";
        }
        observe(std::move(target));
    }
    // The cap is reached: a further observed value is no longer remembered.
    observe("/?Action=DescribeDomains&Signature=observedbeyondcap");
    require(trace.redact_error("echo observedbeyondcap") == "echo observedbeyondcap", "observed values are capped");
    const auto late = secret("late");
    trace.register_secret(late);
    const auto text = trace.redact_error("provider echoed " + late);
    require(text == "provider echoed [REDACTED]", "late credential redacted: " + text);
}

// A credential that cannot be remembered must not leave in clear: the
// whole error is replaced instead.
void verify_incomplete_credentials_fall_back() {
    auto trace = new_recorder();
    for (int index = 0; index < 800; ++index) {
        trace.register_secret(test_support::attempt_trace::canary("bulk"));
    }
    require(trace.redact_error("anything") == service::attempt_trace::redacted_error_fallback,
        "an incomplete credential list replaces the error");
}

// Failures that escaped every attempt are logged with the patterns applied.
void verify_worker_error_patterns() {
    const auto access_key_id = secret("akidloop");
    const auto token = secret("tokenloop");
    const auto text = service::sync_runtime::worker_error("upstream said: server string to sign is:GET&%2F&AccessKeyId%3D" +
                                                          access_key_id + "%26Action%3DDescribeDomains; Authorization: Bearer " +
                                                          token);
    require(contains(text, "upstream said: server string to sign is:[REDACTED]"), "readable worker error: " + text);
    test_support::attempt_trace::require_no_canary(text, canaries, "worker error");
}

}  // namespace

int main() {
    try {
        capturing_buffer log_output;
        auto* const previous = std::cerr.rdbuf(&log_output);
        try {
            const service::logging::session log_session;
            verify_aliyun_signature_mismatch();
            verify_cloudflare_token_echo();
            verify_acme_problem_echo();
        } catch (...) {
            std::cerr.rdbuf(previous);
            throw;
        }
        std::cerr.rdbuf(previous);
        const auto logged = log_output.content();
        require(contains(logged,
                    "DNS sync task aliyun-dns-task failed: SignatureDoesNotMatch: Specified signature is not matched"),
            "aliyun failure logged: " + logged);
        require(contains(logged, "Provider verification marker aliyun-provider-task failed: SignatureDoesNotMatch"),
            "provider failure logged");
        require(contains(logged, "DNS sync task cloudflare-dns-task failed: Invalid format for Authorization header"),
            "cloudflare failure logged");
        require(contains(logged, "Certificate sync marker acme-task failed: ACME 请求失败"), "acme failure logged");
        test_support::attempt_trace::require_no_canary(logged, canaries, "server log");
        verify_credentials_beyond_observed_cap();
        verify_incomplete_credentials_fall_back();
        verify_worker_error_patterns();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Provider error redaction passed (" << canaries.size() << " canaries swept)\n";
    return 0;
}
