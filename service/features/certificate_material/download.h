#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace service::certificate_material {

struct CertificateDownload final {
    std::string filename;
    std::string archive;
};

inline std::string archiveFilename(std::string_view domain) {
    if (domain.starts_with("*.")) {
        domain.remove_prefix(2);
    }
    std::string result;
    result.reserve(domain.size());
    for (const auto character : domain) {
        const auto value = static_cast<unsigned char>(character);
        result.push_back(
            std::isalnum(value) != 0 || character == '.' || character == '-' ? character : '_');
    }
    return result.empty() ? "certificate" : result;
}

} // namespace service::certificate_material
