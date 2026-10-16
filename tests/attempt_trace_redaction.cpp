#include <ruvia/web/model.h>
#include <ruvia/web/model_json.h>
#include <ruvia/web/model_object.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include "service/features/attempt_trace/recorder.h"
#include "support/attempt_trace_fixture.h"

namespace {

using namespace std::string_view_literals;
using service::attempt_trace::outbound_operation;
using service::attempt_trace::recorder;
using service::attempt_trace::step_code;
using test_support::attempt_trace::canary;
using test_support::attempt_trace::exchange;
using test_support::attempt_trace::perform;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error("attempt trace redaction: " + std::string(message));
    }
}

// Typed views of the stored trace, written from the contract
// rather than from the producer's models.
RUVIA_MODEL(pair_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(value, ruvia::string),
    RUVIA_REQUIRED_FIELD(redacted, ruvia::bool_value));
RUVIA_MODEL(body_input, RUVIA_REQUIRED_FIELD(content_type, ruvia::string), RUVIA_REQUIRED_FIELD(format, ruvia::string),
    RUVIA_REQUIRED_FIELD(bytes, ruvia::int64), RUVIA_REQUIRED_FIELD(excerpt, ruvia::string),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value), RUVIA_REQUIRED_FIELD(omitted_reason, ruvia::string));
RUVIA_MODEL(http_input, RUVIA_REQUIRED_FIELD(method, ruvia::string), RUVIA_REQUIRED_FIELD(url, ruvia::string),
    RUVIA_REQUIRED_FIELD(scheme, ruvia::string), RUVIA_REQUIRED_FIELD(host, ruvia::string),
    RUVIA_REQUIRED_FIELD(path, ruvia::string), RUVIA_REQUIRED_FIELD(query, ruvia::array<pair_input>),
    RUVIA_REQUIRED_FIELD(request_headers, ruvia::array<pair_input>),
    RUVIA_REQUIRED_FIELD(request_body, body_input, RUVIA_NULLABLE), RUVIA_REQUIRED_FIELD(status, ruvia::int64),
    RUVIA_REQUIRED_FIELD(transport_error, ruvia::string),
    RUVIA_REQUIRED_FIELD(response_headers, ruvia::array<pair_input>),
    RUVIA_REQUIRED_FIELD(response_body, body_input, RUVIA_NULLABLE));
RUVIA_MODEL(dns_answer_input, RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(ttl, ruvia::int64), RUVIA_REQUIRED_FIELD(data, ruvia::string));
RUVIA_MODEL(dns_input, RUVIA_REQUIRED_FIELD(resolver, ruvia::string), RUVIA_REQUIRED_FIELD(transport, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string), RUVIA_REQUIRED_FIELD(type, ruvia::string),
    RUVIA_REQUIRED_FIELD(rcode, ruvia::string), RUVIA_REQUIRED_FIELD(answers, ruvia::array<dns_answer_input>),
    RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value));
RUVIA_MODEL(call_input, RUVIA_REQUIRED_FIELD(index, ruvia::int64), RUVIA_REQUIRED_FIELD(kind, ruvia::string),
    RUVIA_REQUIRED_FIELD(operation, ruvia::string), RUVIA_REQUIRED_FIELD(subject, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64), RUVIA_REQUIRED_FIELD(repeat_total_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(last_started_at, ruvia::string), RUVIA_REQUIRED_FIELD(provider_request_id, ruvia::string),
    RUVIA_REQUIRED_FIELD(provider_code, ruvia::string), RUVIA_REQUIRED_FIELD(error, ruvia::string),
    RUVIA_REQUIRED_FIELD(http, http_input, RUVIA_NULLABLE), RUVIA_REQUIRED_FIELD(dns, dns_input, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(node_message, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(log_input, RUVIA_REQUIRED_FIELD(at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(level, ruvia::string), RUVIA_REQUIRED_FIELD(message, ruvia::string),
    RUVIA_REQUIRED_FIELD(repeat_count, ruvia::int64));
RUVIA_MODEL(step_input, RUVIA_REQUIRED_FIELD(index, ruvia::int64), RUVIA_REQUIRED_FIELD(code, ruvia::string),
    RUVIA_REQUIRED_FIELD(label, ruvia::string), RUVIA_REQUIRED_FIELD(subject, ruvia::string),
    RUVIA_REQUIRED_FIELD(started_at, ruvia::string), RUVIA_REQUIRED_FIELD(offset_ms, ruvia::int64),
    RUVIA_REQUIRED_FIELD(duration_ms, ruvia::int64), RUVIA_REQUIRED_FIELD(outcome, ruvia::string),
    RUVIA_REQUIRED_FIELD(error, ruvia::string), RUVIA_REQUIRED_FIELD(related_task, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(call_count, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(calls, ruvia::array<call_input>), RUVIA_REQUIRED_FIELD(logs, ruvia::array<log_input>));
RUVIA_MODEL(capture_input, RUVIA_REQUIRED_FIELD(truncated, ruvia::bool_value),
    RUVIA_REQUIRED_FIELD(degraded, ruvia::bool_value), RUVIA_REQUIRED_FIELD(bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_steps, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_calls, ruvia::int64),
    RUVIA_REQUIRED_FIELD(omitted_logs, ruvia::int64), RUVIA_REQUIRED_FIELD(omitted_body_bytes, ruvia::int64),
    RUVIA_REQUIRED_FIELD(redactions, ruvia::int64));
RUVIA_MODEL(document_input, RUVIA_REQUIRED_FIELD(kind, ruvia::string), RUVIA_REQUIRED_FIELD(worker_type, ruvia::string),
    RUVIA_REQUIRED_FIELD(capture, capture_input), RUVIA_REQUIRED_FIELD(steps, ruvia::array<step_input>));

std::vector<std::string> canaries;

std::string remember(std::string value) {
    canaries.push_back(value);
    return value;
}

document_input finish(recorder& trace, std::string* json = nullptr) {
    auto stored = trace.finish();
    require(stored.has_value(), "trace serializes");
    require(stored->trace_json_.size() <= service::attempt_trace::trace_max_bytes, "trace byte cap");
    require(stored->summary_json_.size() <= service::attempt_trace::summary_max_bytes, "summary byte cap");
    require(stored->trace_json_.find("\\u0000") == std::string::npos, "no NUL in stored JSON");
    test_support::attempt_trace::require_no_canary(stored->trace_json_, canaries, "trace");
    test_support::attempt_trace::require_no_canary(stored->summary_json_, canaries, "summary");
    for (const auto* marker : {"PRIVATE KEY", "-----BEGIN", "-----END"}) {
        require(stored->trace_json_.find(marker) == std::string::npos, std::string("no PEM marker ") + marker);
    }
    auto parsed = ruvia::from_json<document_input>(stored->trace_json_);
    require(parsed.has_value(), "trace matches its contract: " + stored->trace_json_.substr(0, 400));
    if (json != nullptr) {
        *json = stored->trace_json_;
    }
    return std::move(*parsed);
}

const call_input& only_call(const document_input& document, std::string_view operation) {
    for (const auto& step : document.get<"steps">()) {
        for (const auto& call : step.get<"calls">()) {
            if (call.get<"operation">().view() == operation) {
                return call;
            }
        }
    }
    throw std::runtime_error("attempt trace redaction: no call " + std::string(operation));
}

const pair_input& pair(const ruvia::array<pair_input>& pairs, std::string_view name) {
    for (const auto& item : pairs) {
        if (item.get<"name">().view() == name) {
            return item;
        }
    }
    throw std::runtime_error("attempt trace redaction: no pair " + std::string(name));
}

const body_input& response_body(const call_input& call) {
    const auto& body = call.get<"http">()->get<"response_body">();
    require(body.has_value(), "response body captured");
    return *body;
}

const body_input& request_body(const call_input& call) {
    const auto& body = call.get<"http">()->get<"request_body">();
    require(body.has_value(), "request body captured");
    return *body;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

recorder new_recorder() {
    return recorder(service::attempt_trace::trace_kind::dns_sync, "dns-sync", std::chrono::steady_clock::now());
}

void verify_aliyun_signed_query_and_echo() {
    const auto access_key_id = remember(canary("akid"));
    const auto signature = remember(canary("sig")) + "+/==";
    canaries.push_back(signature);
    const auto nonce = remember(canary("nonce"));
    exchange call{.call_ = {.origin_alias_ = service::config::aliyun_dns_origin_alias,
                      .operation_ = outbound_operation::aliyun_describe_domains},
        .target_ = "/?AccessKeyId=" + access_key_id +
                   "&Action=DescribeDomains&Format=JSON&PageNumber=1&PageSize=100&Signature=" +
                   test_support::attempt_trace::percent_encoded(signature, true) +
                   "&SignatureMethod=HMAC-SHA1&SignatureNonce=" + nonce +
                   "&SignatureVersion=1.0&Timestamp=2026-10-14T14%3A16%3A44Z&Version=2015-01-09",
        .request_headers_ = {{"accept", "application/json"}},
        .status_ = 400,
        .response_headers_ = {{"Content-Type", "application/json;charset=utf-8"}, {"x-acs-request-id", "5E7C-REQ"}},
        .response_body_ = R"({"RequestId":"5E7C-REQ","HostId":"alidns.aliyuncs.com","Code":"SignatureDoesNotMatch",)"
                          R"("Message":"Specified signature is not matched with our calculation. server string to sign is:GET&%2F&AccessKeyId%3D)" +
                          access_key_id + "%26Action%3DDescribeDomains%26SignatureNonce%3D" + nonce +
                          R"(","Recommend":"https://api.aliyun.com/troubleshoot"})"};
    auto trace = new_recorder();
    {
        auto step = trace.begin_step(step_code::find_zone, "example.com");
        perform(trace, call);
        step.fail("阿里云 DNS 域名查询失败");
    }
    const auto document = finish(trace);
    const auto& recorded = only_call(document, "aliyun.describe_domains");
    const auto& http = *recorded.get<"http">();
    const auto url = http.get<"url">().view();
    require(url.starts_with("https://alidns.aliyuncs.com/?"), "scheme and host come from the origin");
    for (const auto* kept : {"Action=DescribeDomains", "PageSize=100", "SignatureMethod=HMAC-SHA1",
             "Timestamp=2026-10-14T14%3A16%3A44Z", "Version=2015-01-09"}) {
        require(contains(url, kept), std::string("allow-listed parameter kept: ") + kept);
    }
    for (const auto* hidden : {"AccessKeyId=[REDACTED]", "Signature=[REDACTED]", "SignatureNonce=[REDACTED]"}) {
        require(contains(url, hidden), std::string("signed parameter hidden: ") + hidden);
    }
    require(pair(http.get<"query">(), "AccessKeyId").get<"redacted">().value_ &&
                !pair(http.get<"query">(), "Action").get<"redacted">().value_ &&
                pair(http.get<"query">(), "Timestamp").get<"value">().view() == "2026-10-14T14:16:44Z",
        "query pairs are decoded and flagged");
    require(recorded.get<"provider_request_id">().view() == "5E7C-REQ" &&
                recorded.get<"provider_code">().view() == "SignatureDoesNotMatch" &&
                recorded.get<"outcome">().view() == "http_error" && http.get<"status">().value_ == 400,
        "aliyun request id and error code");
    const auto excerpt = response_body(recorded).get<"excerpt">().view();
    require(response_body(recorded).get<"format">().view() == "json" && contains(excerpt, "SignatureDoesNotMatch") &&
                contains(excerpt, "server string to sign is"),
        "error body kept with the echo scrubbed");
    require(document.get<"steps">()[0].get<"outcome">().view() == "failed", "failed step");
}

void verify_cloudflare_token_account_and_cookies() {
    const auto token = remember(canary("token"));
    const auto account = remember(canary("account"));
    const auto cookie = remember(canary("cookie"));
    const auto unknown = remember(canary("unknown"));
    exchange verify{.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                        .operation_ = outbound_operation::cloudflare_verify_token},
        .target_ = "/client/v4/accounts/" + account + "/tokens/verify",
        .request_headers_ = {{"authorization", "Bearer " + token}, {"accept", "application/json"}},
        .status_ = 401,
        .response_headers_ = {{"content-type", "application/json"}, {"cf-ray", "8d1e2f3a4b5c6d7e-HKG"},
            {"set-cookie", "__cf_bm=" + cookie + "; path=/; HttpOnly"}},
        .response_body_ = R"({"success":false,"errors":[{"code":1000,"message":"Invalid API Token )" + token +
                          R"("}],"messages":[],"result":null,"x_unlisted":")" + unknown + R"("})"};
    exchange create{.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                        .operation_ = outbound_operation::cloudflare_create_record,
                        .subject_ = "_acme-challenge TXT"},
        .method_ = "POST",
        .target_ = "/client/v4/zones/0123456789abcdef/dns_records",
        .request_headers_ = {{"authorization", "Bearer " + token}, {"content-type", "application/json"}},
        .request_body_ = R"({"type":"TXT","name":"_acme-challenge","content":"challenge-value","ttl":60,"api_key":")" +
                         unknown + R"("})",
        .has_request_body_ = true,
        .status_ = 200,
        .response_headers_ = {{"content-type", "application/json"}},
        .response_body_ = R"({"success":true,"errors":[],"result":{"id":"rec-1","type":"TXT","name":"_acme-challenge.example.com","content":"challenge-value","ttl":60}})"};
    auto trace = new_recorder();
    trace.register_secret(token);
    trace.register_secret(account);
    {
        auto step = trace.begin_step(step_code::verify_dns_credential);
        perform(trace, verify);
        perform(trace, create);
        step.complete();
    }
    const auto document = finish(trace);
    const auto& verified = only_call(document, "cloudflare.verify_token");
    const auto& http = *verified.get<"http">();
    require(http.get<"path">().view() == "/client/v4/accounts/[REDACTED]/tokens/verify", "account id masked in the path");
    require(pair(http.get<"request_headers">(), "authorization").get<"value">().view() == "[REDACTED]" &&
                pair(http.get<"request_headers">(), "accept").get<"value">().view() == "application/json",
        "authorization hidden, accept kept");
    require(pair(http.get<"response_headers">(), "set-cookie").get<"redacted">().value_ &&
                pair(http.get<"response_headers">(), "cf-ray").get<"value">().view() == "8d1e2f3a4b5c6d7e-HKG",
        "cookie hidden, cf-ray kept");
    require(verified.get<"provider_request_id">().view() == "8d1e2f3a4b5c6d7e-HKG" &&
                verified.get<"provider_code">().view() == "1000",
        "cloudflare request id and error code");
    const auto excerpt = response_body(verified).get<"excerpt">().view();
    require(contains(excerpt, "Invalid API Token [REDACTED]") && contains(excerpt, R"("x_unlisted": "[REDACTED]")"),
        "echoed token and unlisted fields hidden");
    const auto& created = only_call(document, "cloudflare.create_record");
    const auto request = request_body(created).get<"excerpt">().view();
    require(contains(request, R"("content": "challenge-value")") && contains(request, R"("api_key": "[REDACTED]")") &&
                created.get<"subject">().view() == "_acme-challenge TXT",
        "record request body allow-listed");
}

std::string jws(std::string_view protected_json, std::string_view payload_json, std::string_view signature) {
    return R"({"protected":")" + test_support::attempt_trace::base64(protected_json, true) + R"(","payload":")" +
           test_support::attempt_trace::base64(payload_json, true) + R"(","signature":")" + std::string(signature) +
           R"("})";
}

std::pair<std::string, std::string> self_signed_certificate() {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_RSA_gen(2048), &EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), &X509_free);
    require(key && certificate, "test certificate allocated");
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 0x1234ABCD);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400L * 90);
    X509_set_pubkey(certificate.get(), key.get());
    auto* name = X509_get_subject_name(certificate.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("www.example.com"), -1,
        -1, 0);
    X509_set_issuer_name(certificate.get(), name);
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0, "test certificate signed");
    const auto pem = [](auto write) {
        std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), &BIO_free);
        write(output.get());
        char* data = nullptr;
        const auto size = BIO_get_mem_data(output.get(), &data);
        return std::string(data, static_cast<std::size_t>(size));
    };
    return {pem([&](BIO* output) { PEM_write_bio_X509(output, certificate.get()); }),
        pem([&](BIO* output) { PEM_write_bio_PrivateKey(output, key.get(), nullptr, nullptr, 0, nullptr, nullptr); })};
}

void verify_acme_jws_problem_and_chain() {
    const auto nonce = remember(canary("nonce"));
    const auto modulus = remember(canary("modulus"));
    const auto email = remember(canary("email"));
    const auto eab_kid = remember(canary("eabkid"));
    const auto eab_signature = remember(canary("eabsig"));
    const auto signature = remember(canary("jwssig"));
    const auto account_kid = remember(canary("kid"));
    const auto challenge_token = remember(canary("chtoken"));
    const auto csr = remember(canary("csr")) + std::string(600, 'A');
    const auto origin = service::config::lets_encrypt_origin_alias;
    const auto [certificate_pem, private_key_pem] = self_signed_certificate();
    const auto eab_protected = R"({"alg":"HS256","kid":")" + eab_kid + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/new-acct"})";
    std::vector<exchange> calls{
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_new_nonce},
            .method_ = "HEAD",
            .target_ = "/acme/new-nonce",
            .status_ = 200,
            .response_headers_ = {{"Replay-Nonce", nonce}, {"Cache-Control", "no-store"}}},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_new_account},
            .method_ = "POST",
            .target_ = "/acme/new-acct",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256","jwk":{"e":"AQAB","kty":"RSA","n":")" + modulus + R"("},"nonce":")" +
                                     nonce + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/new-acct"})",
                R"({"termsOfServiceAgreed":true,"contact":["mailto:)" + email + R"(@example.com"],"externalAccountBinding":{"protected":")" +
                    test_support::attempt_trace::base64(eab_protected, true) + R"(","payload":"e30","signature":")" +
                    eab_signature + R"("}})",
                signature),
            .has_request_body_ = true,
            .status_ = 201,
            .response_headers_ = {{"content-type", "application/json"},
                {"location", "https://acme-v02.api.letsencrypt.org/acme/acct/42"}},
            .response_body_ = R"({"status":"valid","contact":["mailto:)" + email + R"(@example.com"],"orders":"https://acme-v02.api.letsencrypt.org/acme/acct/42/orders"})"},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_new_order},
            .method_ = "POST",
            .target_ = "/acme/new-order",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256","kid":"https://acme-v02.api.letsencrypt.org/acme/acct/)" + account_kid +
                                     R"(","nonce":")" + nonce + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/new-order"})",
                R"({"identifiers":[{"type":"dns","value":"www.example.com"}]})", signature),
            .has_request_body_ = true,
            .status_ = 201,
            .response_headers_ = {{"content-type", "application/json"}},
            .response_body_ = R"({"status":"pending","identifiers":[{"type":"dns","value":"www.example.com"}],"authorizations":["https://acme-v02.api.letsencrypt.org/acme/authz/1"],"finalize":"https://acme-v02.api.letsencrypt.org/acme/finalize/1"})"},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_authorization},
            .method_ = "POST",
            .target_ = "/acme/authz/1",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256","kid":"x","nonce":")" + nonce + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/authz/1"})",
                "", signature),
            .has_request_body_ = true,
            .status_ = 200,
            .response_headers_ = {{"content-type", "application/json"}},
            .response_body_ = R"({"status":"invalid","identifier":{"type":"dns","value":"www.example.com"},"challenges":[{"type":"dns-01","status":"invalid","url":"https://acme-v02.api.letsencrypt.org/acme/chall/1","token":")" +
                              challenge_token + R"(","error":{"type":"urn:ietf:params:acme:error:unauthorized","detail":"Incorrect TXT record","status":403}}]})"},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_finalize},
            .method_ = "POST",
            .target_ = "/acme/finalize/1",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256","kid":"x","nonce":")" + nonce + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/finalize/1"})",
                R"({"csr":")" + csr + R"("})", signature),
            .has_request_body_ = true,
            .status_ = 400,
            .response_headers_ = {{"content-type", "application/problem+json"}},
            .response_body_ = R"({"type":"urn:ietf:params:acme:error:badNonce","detail":"JWS has an invalid anti-replay nonce: )" +
                              nonce + R"(","status":400})"},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_certificate},
            .method_ = "POST",
            .target_ = "/acme/cert/1",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256","kid":"x","nonce":")" + nonce + R"(","url":"https://acme-v02.api.letsencrypt.org/acme/cert/1"})",
                "", signature),
            .has_request_body_ = true,
            .status_ = 200,
            .response_headers_ = {{"content-type", "application/pem-certificate-chain"}},
            .response_body_ = certificate_pem + certificate_pem},
        {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::acme_certificate},
            .method_ = "POST",
            .target_ = "/acme/cert/2",
            .request_headers_ = {{"content-type", "application/jose+json"}},
            .request_body_ = jws(R"({"alg":"RS256"})", "", signature),
            .has_request_body_ = true,
            .status_ = 200,
            .response_headers_ = {{"content-type", "application/pem-certificate-chain"}},
            .response_body_ = certificate_pem + private_key_pem},
    };
    auto trace = recorder(service::attempt_trace::trace_kind::certificate, "certificate", std::chrono::steady_clock::now());
    trace.register_secret(email);
    {
        auto step = trace.begin_step(step_code::order);
        for (const auto& call : calls) {
            perform(trace, call);
        }
        trace.log(service::attempt_trace::log_level::warning, "private key in a message: " + private_key_pem);
        step.complete();
    }
    std::string json;
    const auto document = finish(trace, &json);
    require(!contains(json, "MII"), "no DER/PEM payload of the key or certificate");
    const auto& nonce_call = only_call(document, "acme.new_nonce");
    require(pair(nonce_call.get<"http">()->get<"response_headers">(), "replay-nonce").get<"redacted">().value_,
        "replay nonce hidden");
    const auto& account = only_call(document, "acme.new_account");
    const auto& account_request = request_body(account);
    require(account_request.get<"format">().view() == "jws_decoded", "jws decoded");
    const auto account_excerpt = account_request.get<"excerpt">().view();
    require(contains(account_excerpt, R"("alg": "RS256")") && contains(account_excerpt, R"("jwk": "[REDACTED]")") &&
                contains(account_excerpt, R"("termsOfServiceAgreed": true)") &&
                contains(account_excerpt, R"("contact": "[REDACTED]")") &&
                contains(account_excerpt, R"("externalAccountBinding": "[REDACTED]")") &&
                !contains(account_excerpt, "nonce") && !contains(account_excerpt, "signature"),
        "account request keeps only alg, url and the terms flag: " + std::string(account_excerpt));
    require(contains(response_body(account).get<"excerpt">().view(), R"("contact": "[REDACTED]")"),
        "account response contact hidden");
    const auto order_excerpt = request_body(only_call(document, "acme.new_order")).get<"excerpt">().view();
    require(contains(order_excerpt, R"("kid": "[REDACTED]")") && contains(order_excerpt, "www.example.com"),
        "order request shows identifiers, hides kid");
    const auto& authorization = only_call(document, "acme.authorization");
    require(contains(request_body(authorization).get<"excerpt">().view(), "(POST-as-GET)"), "POST-as-GET shown");
    const auto authorization_excerpt = response_body(authorization).get<"excerpt">().view();
    require(contains(authorization_excerpt, R"("token": "[REDACTED]")") &&
                contains(authorization_excerpt, "Incorrect TXT record"),
        "challenge token hidden, problem detail kept");
    const auto& finalize = only_call(document, "acme.finalize");
    require(contains(request_body(finalize).get<"excerpt">().view(),
                "[CSR " + std::to_string(csr.size()) + " bytes]"),
        "CSR shown as its size");
    require(finalize.get<"provider_code">().view() == "urn:ietf:params:acme:error:badNonce" &&
                contains(response_body(finalize).get<"excerpt">().view(), "anti-replay nonce: [REDACTED]"),
        "ACME problem type and scrubbed detail");
    bool chain = false;
    bool key = false;
    for (const auto& step : document.get<"steps">()) {
        for (const auto& call : step.get<"calls">()) {
            if (call.get<"operation">().view() != "acme.certificate") {
                continue;
            }
            const auto& body = response_body(call);
            if (body.get<"format">().view() == "pem_summary") {
                const auto excerpt = body.get<"excerpt">().view();
                chain = contains(excerpt, R"("subject": "CN=www.example.com")") &&
                        contains(excerpt, R"("serial_number": "1234ABCD")") && contains(excerpt, R"("not_after": ")");
            } else if (body.get<"omitted_reason">().view() == "secret_material") {
                key = body.get<"excerpt">().view().empty();
            }
        }
    }
    require(chain, "certificate chain reduced to subject, issuer, serial and expiry");
    require(key, "a body with a private key is not stored");
    require(document.get<"capture">().get<"redactions">().value_ > 10, "redactions counted");
}

void verify_zerossl_credentials() {
    const auto access_key = remember(canary("access"));
    const auto email = remember(canary("mail"));
    const auto kid = remember(canary("zkid"));
    const auto hmac = remember(canary("hmac"));
    const auto origin = service::config::zero_ssl_api_origin_alias;
    auto trace = recorder(service::attempt_trace::trace_kind::provider, "provider-verification",
        std::chrono::steady_clock::now());
    trace.register_secret(email + "@example.com");
    {
        auto step = trace.begin_step(step_code::load_eab);
        perform(trace, {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::zerossl_eab_by_access_key},
                           .method_ = "POST",
                           .target_ = "/acme/eab-credentials?access_key=" + access_key,
                           .request_headers_ = {{"accept", "application/json"}},
                           .status_ = 200,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = R"({"success":true,"eab_kid":")" + kid + R"(","eab_hmac_key":")" + hmac + R"("})"});
        perform(trace, {.call_ = {.origin_alias_ = origin, .operation_ = outbound_operation::zerossl_eab_by_email},
                           .method_ = "POST",
                           .target_ = "/acme/eab-credentials-email",
                           .request_headers_ = {{"content-type", "application/x-www-form-urlencoded"}},
                           .request_body_ = "email=" + test_support::attempt_trace::percent_encoded(email + "@example.com", true),
                           .has_request_body_ = true,
                           .status_ = 400,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = R"({"success":false,"error":{"code":2900,"type":"invalid_email","info":"bad )" + email + R"(@example.com"}})"});
        trace.register_secret(kid);
        trace.register_secret(hmac);
        trace.log(service::attempt_trace::log_level::info, "eab " + hmac);
        step.complete();
    }
    const auto document = finish(trace);
    const auto& by_key = only_call(document, "zerossl.eab_by_access_key");
    require(contains(by_key.get<"http">()->get<"url">().view(), "access_key=[REDACTED]"), "access key hidden");
    const auto key_excerpt = response_body(by_key).get<"excerpt">().view();
    require(contains(key_excerpt, R"("eab_kid": "[REDACTED]")") && contains(key_excerpt, R"("eab_hmac_key": "[REDACTED]")") &&
                contains(key_excerpt, R"("success": true)"),
        "EAB credentials hidden");
    const auto& by_email = only_call(document, "zerossl.eab_by_email");
    require(request_body(by_email).get<"excerpt">().view() == "email=[REDACTED]" &&
                request_body(by_email).get<"format">().view() == "form",
        "form email hidden");
    require(by_email.get<"provider_code">().view() == "invalid_email", "ZeroSSL error type");
}

void verify_free_text() {
    const auto registered = remember(canary("registered"));
    const auto bearer = remember(canary("bearer"));
    const auto jwt_body = remember(canary("jwt"));
    const auto signed_value = remember(canary("signed"));
    const auto key_body = remember(canary("keybody"));
    const auto failure = remember(canary("failure"));
    auto trace = new_recorder();
    trace.register_secret(registered);
    trace.register_secret("abc12");
    {
        auto step = trace.begin_step(step_code::reconcile_records, "www A");
        trace.log(service::attempt_trace::log_level::info,
            "raw " + registered + " base64 " + test_support::attempt_trace::base64(registered, false) + " url " +
                test_support::attempt_trace::base64(registered, true) + " percent " +
                test_support::attempt_trace::percent_encoded(registered + "/", false));
        trace.log(service::attempt_trace::log_level::info, "Authorization: Bearer " + bearer);
        trace.log(service::attempt_trace::log_level::info, "jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIi" + jwt_body + ".c2ln");
        trace.log(service::attempt_trace::log_level::info,
            "string to sign GET&%2F&AccessKeyId%3D" + signed_value + "%26Action%3DX and echo AccessKeyId%3D" + signed_value +
                "%26Action and Signature=" + signed_value);
        trace.log(service::attempt_trace::log_level::error,
            "-----BEGIN RSA PRIVATE KEY-----\n" + key_body + "\n-----END RSA PRIVATE KEY-----");
        trace.log(service::attempt_trace::log_level::info, "short secret abc12 stays");
        trace.log(service::attempt_trace::log_level::info, std::string("nul\0byte", 8));
        trace.log(service::attempt_trace::log_level::info, "repeated");
        trace.log(service::attempt_trace::log_level::info, "repeated");
    }
    trace.mark_failure("request failed: token=" + failure);
    const auto document = finish(trace);
    const auto& step = document.get<"steps">()[0];
    require(step.get<"outcome">().view() == "failed" && contains(step.get<"error">().view(), "token=[REDACTED]"),
        "unclosed scope fails and receives the scrubbed attempt error");
    const auto& logs = step.get<"logs">();
    require(contains(logs[0].get<"message">().view(), "raw [REDACTED] base64 [REDACTED] url [REDACTED] percent [REDACTED]"),
        "registered secret replaced in every encoding: " + std::string(logs[0].get<"message">().view()));
    require(logs[1].get<"message">().view() == "Authorization: Bearer [REDACTED]", "bearer pattern");
    require(logs[2].get<"message">().view() == "jwt [REDACTED]", "jwt pattern");
    require(logs[3].get<"message">().view() ==
                "string to sign [REDACTED] and echo AccessKeyId%3D[REDACTED]%26Action and Signature=[REDACTED]",
        "signed string echo dropped, signed parameters replaced: " + std::string(logs[3].get<"message">().view()));
    require(logs[4].get<"message">().view() == "[REDACTED]", "private key block");
    require(contains(logs[5].get<"message">().view(), "abc12"), "secrets shorter than 8 bytes are not replaced");
    require(logs[6].get<"message">().view() == "nul byte", "control characters become spaces");
    require(logs[7].get<"repeat_count">().value_ == 2, "adjacent identical lines fold");
}

void verify_json_bounds_and_text() {
    const auto deep = remember(canary("deep"));
    std::string nested = R"({"result":[)";
    for (int index = 0; index < 80; ++index) {
        nested += (index == 0 ? "" : ",") + std::string(R"({"id":"r)") + std::to_string(index) +
                  R"(","name":"a.example.com","type":"A","content":"192.0.2.1","ttl":300,"meta":{"secret":")" + deep + R"("}})";
    }
    nested += R"(],"success":true})";
    std::string wide;
    while (wide.size() < 20000) {
        wide += "错误信息";
    }
    auto trace = new_recorder();
    {
        auto step = trace.begin_step(step_code::list_records);
        perform(trace, {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                            .operation_ = outbound_operation::cloudflare_list_records},
                           .target_ = "/client/v4/zones/z/dns_records?page=1&per_page=100",
                           .status_ = 200,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = nested});
        perform(trace, {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                            .operation_ = outbound_operation::cloudflare_list_zones},
                           .target_ = "/client/v4/zones?page=2",
                           .status_ = 502,
                           .response_headers_ = {{"content-type", "text/html"}},
                           .response_body_ = wide});
        perform(trace, {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                            .operation_ = outbound_operation::cloudflare_find_zone},
                           .target_ = "/client/v4/zones?name=example.com",
                           .status_ = 200,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = R"({"result": [ not json )"});
        perform(trace, {.call_ = {.origin_alias_ = service::config::cloudflare_origin_alias,
                            .operation_ = outbound_operation::cloudflare_find_zone},
                           .target_ = "/client/v4/zones?name=example.org",
                           .failure_ = ruvia::http_client_error::code_type::timeout});
        step.complete();
    }
    const auto document = finish(trace);
    const auto& list = only_call(document, "cloudflare.list_records");
    const auto excerpt = response_body(list).get<"excerpt">().view();
    require(contains(excerpt, R"("meta": "[REDACTED]")") && contains(excerpt, R"("content": "192.0.2.1")"),
        "unlisted objects hidden, listed fields kept");
    require(response_body(list).get<"truncated">().value_ &&
                excerpt.size() <= service::attempt_trace::response_excerpt_bytes,
        "excerpt cut at its cap: " + std::to_string(excerpt.size()) + " " +
            std::to_string(response_body(list).get<"truncated">().value_) + " " + std::string(excerpt.substr(0, 300)));
    const auto& html = only_call(document, "cloudflare.list_zones");
    const auto html_excerpt = response_body(html).get<"excerpt">().view();
    require(response_body(html).get<"format">().view() == "text" && response_body(html).get<"truncated">().value_ &&
                html_excerpt.size() <= service::attempt_trace::response_excerpt_bytes &&
                response_body(html).get<"bytes">().value_ == static_cast<std::int64_t>(wide.size()),
        "text excerpt cut, original size reported");
    for (std::size_t index = 0; index < html_excerpt.size();) {
        const auto length = service::utils::utf8_sequence_length(html_excerpt, index);
        require(length != 0, "excerpt cut on a UTF-8 boundary");
        index += length;
    }
    bool invalid_json = false;
    bool timeout = false;
    for (const auto& step : document.get<"steps">()) {
        for (const auto& call : step.get<"calls">()) {
            if (call.get<"operation">().view() != "cloudflare.find_zone") {
                continue;
            }
            if (call.get<"outcome">().view() == "transport_error") {
                timeout = call.get<"error">().view() == "timeout" && call.get<"http">()->get<"status">().value_ == 0 &&
                          call.get<"http">()->get<"transport_error">().view() == "timeout";
            } else {
                invalid_json = response_body(call).get<"format">().view() == "text";
            }
        }
    }
    require(invalid_json, "invalid JSON is stored as scanned text");
    require(timeout, "transport failure recorded");
}

void verify_dns_over_https() {
    auto trace = recorder(service::attempt_trace::trace_kind::website_domains, "website-dispatch",
        std::chrono::steady_clock::now());
    {
        auto step = trace.begin_step(step_code::check_domain, "www.example.com");
        perform(trace, {.call_ = {.origin_alias_ = service::config::ali_dns_doh_origin_alias,
                            .operation_ = outbound_operation::doh_resolve,
                            .subject_ = "www.example.com"},
                           .target_ = "/resolve?name=www.example.com&type=CNAME&cd=0&do=0",
                           .request_headers_ = {{"accept", "application/dns-json"}},
                           .status_ = 200,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = R"({"Status":0,"TC":false,"Question":[{"name":"www.example.com.","type":5}],"Answer":[{"name":"www.example.com.","type":5,"TTL":600,"data":"edge.example.net."}]})"});
        perform(trace, {.call_ = {.origin_alias_ = service::config::ali_dns_doh_origin_alias,
                            .operation_ = outbound_operation::doh_resolve,
                            .subject_ = "missing.example.com"},
                           .target_ = "/resolve?name=missing.example.com&type=CNAME&cd=0&do=0",
                           .status_ = 200,
                           .response_headers_ = {{"content-type", "application/json"}},
                           .response_body_ = R"({"Status":3,"Question":[{"name":"missing.example.com.","type":5}]})"});
        perform(trace, {.call_ = {.origin_alias_ = service::config::ali_dns_doh_origin_alias,
                            .operation_ = outbound_operation::doh_resolve,
                            .subject_ = "down.example.com"},
                           .target_ = "/resolve?name=down.example.com&type=CNAME&cd=0&do=0",
                           .failure_ = ruvia::http_client_error::code_type::connect_failed});
        step.complete();
    }
    const auto document = finish(trace);
    const auto& calls = document.get<"steps">()[0].get<"calls">();
    require(calls.size() == 3 && calls[0].get<"kind">().view() == "dns", "DoH calls are DNS queries");
    const auto& dns = *calls[0].get<"dns">();
    require(dns.get<"resolver">().view() == "dns.alidns.com" && dns.get<"name">().view() == "www.example.com" &&
                dns.get<"type">().view() == "CNAME" && dns.get<"rcode">().view() == "NOERROR" &&
                dns.get<"answers">().size() == 1 && dns.get<"answers">()[0].get<"type">().view() == "CNAME" &&
                dns.get<"answers">()[0].get<"data">().view() == "edge.example.net." &&
                dns.get<"answers">()[0].get<"ttl">().value_ == 600,
        "DNS query, rcode and answers");
    require(calls[1].get<"dns">()->get<"rcode">().view() == "NXDOMAIN" &&
                calls[1].get<"provider_code">().view() == "NXDOMAIN",
        "DNS error rcode");
    const auto& unanswered = calls[2].get<"dns">();
    require(calls[2].get<"outcome">().view() == "transport_error" && unanswered.has_value() &&
                unanswered->get<"name">().view() == "down.example.com" && unanswered->get<"type">().view() == "CNAME" &&
                unanswered->get<"rcode">().view().empty() && unanswered->get<"answers">().empty(),
        "a failed DNS query still records the query");
}

void verify_policy_table() {
    std::set<std::string_view> codes;
    for (const auto& policy : service::attempt_trace::outbound_policies) {
        require(&service::attempt_trace::policy_for(policy.operation_) == &policy, "operation maps to its policy");
        require(codes.insert(policy.code_).second, "operation codes are unique");
        for (const auto forbidden : {"AccessKeyId"sv, "Signature"sv, "SignatureNonce"sv, "access_key"sv, "token"sv,
                 "account.id"sv}) {
            require(!service::attempt_trace::text_detail::contains(policy.query_, forbidden),
                "credential parameter is never allow-listed");
        }
        for (const auto forbidden : {"eab_hmac_key"sv, "eab_kid"sv, "token"sv, "contact"sv, "key"sv, "csr"sv,
                 "challenges[].token"sv, "externalAccountBinding"sv}) {
            require(!service::attempt_trace::text_detail::contains(policy.response_fields_, forbidden) &&
                        !service::attempt_trace::text_detail::contains(policy.request_fields_, forbidden),
                "credential field is never allow-listed");
        }
    }
    for (const auto header : {"authorization"sv, "cookie"sv, "x-auth-key"sv}) {
        require(!service::attempt_trace::text_detail::contains(service::attempt_trace::request_header_allow_list, header),
            "credential request header is never allow-listed");
    }
    for (const auto header : {"set-cookie"sv, "replay-nonce"sv}) {
        require(!service::attempt_trace::text_detail::contains(service::attempt_trace::response_header_allow_list, header),
            "credential response header is never allow-listed");
    }
}

}  // namespace

int main() {
    try {
        test_support::attempt_trace::ensure_outbound_origins();
        verify_policy_table();
        verify_aliyun_signed_query_and_echo();
        verify_cloudflare_token_account_and_cookies();
        verify_acme_jws_problem_and_chain();
        verify_zerossl_credentials();
        verify_free_text();
        verify_json_bounds_and_text();
        verify_dns_over_https();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Attempt trace redaction passed (" << canaries.size() << " canaries swept)\n";
    return 0;
}
