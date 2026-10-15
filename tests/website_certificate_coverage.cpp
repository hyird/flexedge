#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "service/domains/website/certificate_coverage.h"

namespace {
void require(bool condition) {
    if (!condition) {
        throw std::runtime_error("website certificate coverage invariant failed");
    }
}

service::website_config::website_domain_data domain(std::string hostname) {
    service::website_config::website_domain_data result{};
    result.hostname_ = std::move(hostname);
    return result;
}
}  // namespace

int main() try {
    using service::common::certificate_covers_hostname;
    // Exact names match case-insensitively and without trailing dots.
    require(certificate_covers_hostname("a-z.xin", "a-z.xin"));
    require(certificate_covers_hostname("A-Z.XIN.", "a-z.xin"));
    require(certificate_covers_hostname("a-z.xin", "A-Z.Xin."));
    require(!certificate_covers_hostname("a-z.xin", "t.a-z.xin"));
    // A wildcard name covers exactly one extra left-most label.
    require(certificate_covers_hostname("*.a-z.xin", "t.a-z.xin"));
    require(certificate_covers_hostname("*.A-Z.xin", "T.a-z.xin."));
    require(!certificate_covers_hostname("*.a-z.xin", "a-z.xin"));
    require(!certificate_covers_hostname("*.a-z.xin", "x.t.a-z.xin"));
    require(!certificate_covers_hostname("*.a-z.xin", "ta-z.xin"));
    // A wildcard website hostname is covered only by the identical wildcard name.
    require(certificate_covers_hostname("*.a-z.xin", "*.a-z.xin"));
    require(!certificate_covers_hostname("*.a-z.xin", "*.t.a-z.xin"));
    require(!certificate_covers_hostname("t.a-z.xin", "*.a-z.xin"));
    require(!certificate_covers_hostname("a-z.xin", "*.a-z.xin"));
    require(!certificate_covers_hostname("", "a-z.xin"));

    using service::website::certificate_reference;
    using service::website::find_unmatched_certificate;
    const std::array domains{domain("a-z.xin"), domain("t.a-z.xin")};
    const std::vector<certificate_reference> partial{
        {.id_ = "apex", .names_ = {"a-z.xin"}},
        {.id_ = "multi", .names_ = {"other.example", "T.A-Z.XIN."}},
    };
    // Each certificate matches one hostname; partial coverage of the set is allowed.
    require(find_unmatched_certificate(partial, domains) == nullptr);
    require(find_unmatched_certificate(partial, std::array{domain("x.a-z.xin")}) != nullptr);
    const std::vector<certificate_reference> with_unmatched{
        {.id_ = "wildcard", .names_ = {"*.a-z.xin"}},
        {.id_ = "deep", .names_ = {"*.t.a-z.xin"}},
        {.id_ = "unnamed", .names_ = {}},
    };
    const auto* unmatched = find_unmatched_certificate(with_unmatched, domains);
    require(unmatched && unmatched->id_ == "deep");
    require(!find_unmatched_certificate(std::vector<certificate_reference>{}, domains));
    // Without hostnames every selected certificate is unmatched.
    require(find_unmatched_certificate(partial, {}) == &partial.front());
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
