#pragma once

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string_view>

#include "node/runtime/secure_file.h"

namespace flexedge::testing {
// This fixture is used only by Linux processes started with dns_resolver_stub preloaded.
class dns_answers final {
public:
    explicit dns_answers(std::filesystem::path path)
        : path_(std::move(path)) {
        update("127.0.0.1");
        if (::setenv("FLEXEDGE_TEST_DNS_FILE", path_.c_str(), 1) != 0) {
            throw std::runtime_error("could not configure DNS test fixture");
        }
    }
    ~dns_answers() {
        ::unsetenv("FLEXEDGE_TEST_DNS_FILE");
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    void update(std::string_view addresses) const {
        node::write_secure_file_atomic(path_, addresses);
    }

private:
    std::filesystem::path path_;
};
}  // namespace flexedge::testing
