#include <iostream>
#include <string>
#include <string_view>

#include "service/features/geoip/xdb_database.h"

namespace {

struct arguments final {
    std::string ipv4_;
    std::string ipv6_;
    std::string ip_;
};

[[nodiscard]] bool parse_arguments(int argc, char** argv, arguments& result) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            return false;
        }
        const std::string value{argv[++index]};
        if (argument == "--v4") {
            result.ipv4_ = value;
        } else if (argument == "--v6") {
            result.ipv6_ = value;
        } else if (argument == "--ip") {
            result.ip_ = value;
        } else {
            return false;
        }
    }
    return !result.ip_.empty() && (!result.ipv4_.empty() || !result.ipv6_.empty());
}

}  // namespace

int main(int argc, char** argv) {
    arguments arguments;
    if (!parse_arguments(argc, argv, arguments)) {
        std::cerr << "usage: xdb_lookup [--v4 <ipv4.xdb>] [--v6 <ipv6.xdb>] --ip <address>\n";
        return 1;
    }
    const service::geoip::xdb_database database(arguments.ipv4_, arguments.ipv6_);
    if (!database.available()) {
        std::cerr << "could not open XDB database\n";
        return 1;
    }
    const auto location = database.lookup(arguments.ip_);
    if (!location) {
        std::cerr << "no location data for supplied IP\n";
        return 2;
    }
    std::cout << location->display_ << '\n';
    return 0;
}
