#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <system_error>

#include <asio/ip/address.hpp>

namespace service::website_dns {

// A provider that flattens a CNAME (Cloudflare always does at the zone apex, and for every
// CNAME when the zone enables it) answers the target's A/AAAA records instead of the CNAME.
// Those answers prove a hosted domain reaches the website's cluster when there is at least
// one and every one is an endpoint address of the cluster's nodes: DNS sync publishes exactly
// those addresses at the cluster access hostname, on every resolution line, so the check holds
// whichever line the flattening resolver was answered from.
[[nodiscard]] inline bool matches_flattened_cname(std::span<const std::string> answers,
    std::span<const std::string> cluster_addresses) {
    if (answers.empty()) {
        return false;
    }
    return std::ranges::all_of(answers, [&](const std::string& answer) {
        std::error_code answer_error;
        const auto address = asio::ip::make_address(answer, answer_error);
        return !answer_error && std::ranges::any_of(cluster_addresses, [&](const std::string& endpoint) {
            std::error_code endpoint_error;
            const auto expected = asio::ip::make_address(endpoint, endpoint_error);
            return !endpoint_error && expected == address;
        });
    });
}

}  // namespace service::website_dns
