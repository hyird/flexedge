#include <stdexcept>
#include <iostream>
#include <string>
#include <ruvia/web/ModelJson.h>
#include "service/domains/node/node.schema.h"

bool unique(std::string first, std::string second,
            std::string firstId = "aaaaaaaa-0000-0000-0000-000000000001",
            std::string secondId = "aaaaaaaa-0000-0000-0000-000000000002") {
    const auto input = ruvia::fromJson<service::node_config::NodeConfigInput>(
        "{\"endpoints\":[{\"id\":\"" + firstId + "\",\"ip_address\":\"" + first +
        "\"},{\"id\":\"" + secondId + "\",\"ip_address\":\"" + second + "\"}]}");
    if (!input || !input->get<"endpoints">()) throw std::runtime_error("invalid fixture");
    return service::node::hasUniqueEndpoints(*input->get<"endpoints">());
}

int main() try {
    if (service::common::parseIpAddress("fe80::1%1") ||
        service::common::parseIpAddress("fe80::1%0"))
        throw std::runtime_error("scoped IPv6 endpoint admitted");
    if (!service::common::parseIpAddress("fe80::1"))
        throw std::runtime_error("unscoped IPv6 endpoint rejected");
    if (unique("192.0.2.1", "192.0.2.2",
               "aaaaaaaa-0000-0000-0000-000000000001",
               "AAAAAAAA-0000-0000-0000-000000000001"))
        throw std::runtime_error("equivalent UUID endpoints admitted");
    if (unique("2001:db8::1", "2001:0DB8:0:0:0:0:0:1"))
        throw std::runtime_error("equivalent IPv6 endpoints admitted");
    if (!unique("2001:db8::1", "2001:db8::2"))
        throw std::runtime_error("distinct IPv6 endpoints rejected");
    if (unique("192.0.2.1", "192.0.2.1"))
        throw std::runtime_error("duplicate IPv4 endpoints admitted");
    if (!unique("192.0.2.1", "::ffff:192.0.2.1"))
        throw std::runtime_error("different address families conflated");
} catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
}
