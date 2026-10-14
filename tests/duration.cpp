#include "service/config/duration.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

int main() {
    using service::config::parse_positive_duration;
    for (const auto value : {"60", "60s", "1m"}) {
        if (parse_positive_duration(value, "test") != std::chrono::seconds(60)) {
            return 1;
        }
    }
    if (parse_positive_duration("1h", "test").count() != 3600 ||
        parse_positive_duration("7d", "test").count() != 604800) {
        return 2;
    }
    for (const auto value : {"", "0", "-1", "1x", "m", "1.5h", "1h ",
             "9223372036854775808", "9223372036854775807d"}) {
        try {
            (void)parse_positive_duration(value, "test");
            std::cerr << "accepted invalid duration: " << value << '\n';
            return 3;
        } catch (const std::runtime_error&) {
        }
    }
    return 0;
}
