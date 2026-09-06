#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "service/features/geoip/xdb_database.h"

namespace {

struct Arguments final {
    std::string ipv4;
    std::string ipv6;
    std::string ip;
};

[[nodiscard]] bool setEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
    return _putenv_s(name, value.c_str()) == 0;
#else
    return setenv(name, value.c_str(), 1) == 0;
#endif
}

[[nodiscard]] bool parseArguments(int argc, char** argv, Arguments& result) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            return false;
        }
        const std::string value{argv[++index]};
        if (argument == "--v4") {
            result.ipv4 = value;
        } else if (argument == "--v6") {
            result.ipv6 = value;
        } else if (argument == "--ip") {
            result.ip = value;
        } else {
            return false;
        }
    }
    return !result.ip.empty() && (!result.ipv4.empty() || !result.ipv6.empty());
}

} // namespace

int main(int argc, char** argv) {
    Arguments arguments;
    if (!parseArguments(argc, argv, arguments)) {
        std::cerr << "usage: xdb_lookup [--v4 <ipv4.xdb>] [--v6 <ipv6.xdb>] --ip <address>\n";
        return 1;
    }
    if ((!arguments.ipv4.empty() &&
         !setEnvironment("FLEXEDGE_XDB_V4_PATH", arguments.ipv4)) ||
        (!arguments.ipv6.empty() && !setEnvironment("FLEXEDGE_XDB_V6_PATH", arguments.ipv6))) {
        std::cerr << "could not configure XDB runtime paths\n";
        return 1;
    }
    const auto& database = service::geoip::xdbDatabase();
    if (!database.available()) {
        std::cerr << "could not open XDB database\n";
        return 1;
    }
    const auto location = database.lookup(arguments.ip);
    if (!location) {
        std::cerr << "no location data for supplied IP\n";
        return 2;
    }
    std::cout << location->display << '\n';
    return 0;
}
