#pragma once

#include <ruvia/http/http_client.h>
#include <ruvia/web/model_object.h>

#include <array>
#include <cctype>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/config/outbound.h"
#include "service/features/attempt_trace/recorder.h"

// Helpers shared by the attempt trace tests: secret canaries, an exhaustive
// check that no encoding of a canary reached stored text, and one simulated
// outbound HTTP exchange through the recorder exactly as send_buffered does.
namespace test_support::attempt_trace {

inline void ensure_outbound_origins() {
    static const bool configured = [] {
        service::config::configure_outbound_origins(service::config::make_outbound_origins(""));
        return true;
    }();
    (void)configured;
}

// A random secret of letters and digits that cannot occur by chance.
inline std::string canary(std::string_view label) {
    static std::mt19937_64 generator(std::random_device{}());
    constexpr std::string_view alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    std::string value = "cnry" + std::string(label);
    std::uniform_int_distribution<std::size_t> pick(0, alphabet.size() - 1);
    for (int index = 0; index < 28; ++index) {
        value.push_back(alphabet[pick(generator)]);
    }
    return value;
}

inline std::string percent_encoded(std::string_view value, bool upper) {
    constexpr std::string_view upper_digits = "0123456789ABCDEF";
    constexpr std::string_view lower_digits = "0123456789abcdef";
    const auto digits = upper ? upper_digits : lower_digits;
    std::string result;
    for (const auto raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0 || raw == '-' || raw == '.' || raw == '_' || raw == '~') {
            result.push_back(raw);
        } else {
            result.push_back('%');
            result.push_back(digits[ch >> 4]);
            result.push_back(digits[ch & 0x0F]);
        }
    }
    return result;
}

inline std::string base64(std::string_view value, bool url) {
    const std::string_view alphabet = url ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                                          : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    unsigned buffer = 0;
    int bits = 0;
    for (const auto raw : value) {
        buffer = (buffer << 8) | static_cast<unsigned char>(raw);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            result.push_back(alphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        result.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    while (!url && result.size() % 4 != 0) {
        result.push_back('=');
    }
    return result;
}

// Every form in which `secret` could have been stored: raw, percent-encoded
// (both hex cases), base64, base64url, and the 12-byte core of each so a
// partially cut copy is caught too.
inline std::vector<std::string> encodings(std::string_view secret) {
    std::vector<std::string> result{std::string(secret), percent_encoded(secret, true),
        percent_encoded(secret, false), base64(secret, false), base64(secret, true)};
    if (secret.size() > 16) {
        result.emplace_back(secret.substr(4, 12));
    }
    return result;
}

// Throws when any encoding of any canary occurs in `stored`.
inline void require_no_canary(std::string_view stored, std::span<const std::string> canaries, std::string_view where) {
    for (const auto& secret : canaries) {
        for (const auto& encoded : encodings(secret)) {
            if (stored.find(encoded) != std::string_view::npos) {
                throw std::runtime_error(std::string(where) + " leaks canary " + secret + " as " + encoded);
            }
        }
    }
}

struct exchange final {
    service::attempt_trace::outbound_call call_{};
    std::string method_{"GET"};
    std::string target_{"/"};
    std::vector<std::pair<std::string, std::string>> request_headers_{};
    std::string request_body_{};
    bool has_request_body_{};
    int status_{200};
    std::vector<std::pair<std::string, std::string>> response_headers_{};
    std::string response_body_{};
    // Set to simulate a transport failure instead of a response.
    std::optional<ruvia::http_client_error::code_type> failure_{};
};

// Runs one exchange through the recorder the way send_buffered does.
inline void perform(service::attempt_trace::recorder& trace, const exchange& value) {
    std::vector<ruvia::http_header_view> headers;
    for (const auto& [name, header_value] : value.request_headers_) {
        headers.emplace_back(name, header_value);
    }
    const ruvia::http_client_request_view request{
        .method_ = value.method_,
        .target_ = value.target_,
        .headers_ = headers,
        .content_ = value.has_request_body_ ? ruvia::http_client_request_content_view::bytes(value.request_body_)
                                            : ruvia::http_client_request_content_view::none(),
    };
    const auto token = trace.begin_http(value.call_, request);
    if (value.failure_) {
        trace.fail_http(token, *value.failure_);
        return;
    }
    std::vector<service::attempt_trace::header_entry> response_headers;
    for (const auto& [name, header_value] : value.response_headers_) {
        response_headers.push_back({.name_ = name, .value_ = header_value});
    }
    trace.complete_http(token, value.status_, response_headers, value.response_body_);
}

}  // namespace test_support::attempt_trace
