#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "service/features/website_dns/flattening.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(std::string("website DNS flattening: ") + message);
    }
}
}  // namespace

int main() try {
    using service::website_dns::matches_flattened_cname;
    const std::vector<std::string> cluster{"192.0.2.10", "192.0.2.11", "2001:db8::10"};

    // Cloudflare flattens an apex CNAME into the target's addresses; any subset of the
    // cluster endpoints (one resolution line's answer) verifies the hosted domain.
    require(matches_flattened_cname(std::vector<std::string>{"192.0.2.10"}, cluster), "single node");
    require(matches_flattened_cname(std::vector<std::string>{"192.0.2.11", "192.0.2.10", "2001:db8::10"}, cluster),
        "every node, any order");
    require(matches_flattened_cname(std::vector<std::string>{"2001:0db8:0000::0010"}, cluster),
        "IPv6 compares by address, not spelling");

    // An answer outside the cluster, no answer, or a non-address never verifies it.
    require(!matches_flattened_cname(std::vector<std::string>{"192.0.2.10", "198.51.100.7"}, cluster),
        "foreign address");
    require(!matches_flattened_cname(std::vector<std::string>{}, cluster), "no answer");
    require(!matches_flattened_cname(std::vector<std::string>{"edge.example.net"}, cluster), "not an address");
    require(!matches_flattened_cname(std::vector<std::string>{"192.0.2.10"}, std::vector<std::string>{}),
        "cluster without published nodes");
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
