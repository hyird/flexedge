#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "service/features/node_release/artifact.h"

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition))                                                                          \
            throw std::runtime_error("requirement failed: " #condition);                           \
    } while (false)

namespace {

class TemporaryDirectory final {
  public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("flexedge-node-release-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("could not write test release artifact");
    }
}

[[nodiscard]] std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read test release artifact");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void replaceFile(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    std::filesystem::remove(destination);
#endif
    std::filesystem::rename(source, destination);
}

} // namespace

int main() {
    try {
        TemporaryDirectory directory;
        const auto binaryPath = directory.path() / "node";
        const auto installerPath = directory.path() / "install-node.sh";
        const auto manifestPath = directory.path() / "node-release.manifest";
        writeFile(binaryPath, "node-release-one");
        writeFile(installerPath, "installer-release-one");
        const auto firstDigest = flexedge::node::binarySha256(binaryPath);
        writeFile(manifestPath,
                  "flexedge-node-release-v1\r\nversion=1.0.0\r\nsha256=" + firstDigest + "\r\n");

        service::node_release::Catalog catalog(std::chrono::nanoseconds::zero());
        catalog.configure(binaryPath, installerPath, manifestPath);
        auto first = catalog.current();
        REQUIRE(first->binary().digest() == firstDigest);
        REQUIRE(first->version() == "1.0.0");
        const auto firstBinaryPath = first->binary().path();
        const auto firstInstallerPath = first->installer().path();
        REQUIRE(readFile(firstBinaryPath) == "node-release-one");
        REQUIRE(readFile(firstInstallerPath) == "installer-release-one");

        const auto nextBinaryPath = directory.path() / "node.next";
        writeFile(nextBinaryPath, "node-release-two");
        replaceFile(nextBinaryPath, binaryPath);
        const auto stalled = catalog.current();
        REQUIRE(stalled->binary().digest() == firstDigest);
        REQUIRE(stalled->version() == "1.0.0");
        const auto secondDigest = flexedge::node::binarySha256(binaryPath);
        const auto nextManifestPath = directory.path() / "node-release.manifest.next";
        writeFile(nextManifestPath,
                  "flexedge-node-release-v1\nversion=2.0.0\nsha256=" + secondDigest + "\n");
        replaceFile(nextManifestPath, manifestPath);
        first.reset();
        const auto second = catalog.current();
        REQUIRE(second->binary().digest() == secondDigest);
        REQUIRE(second->version() == "2.0.0");
        REQUIRE(readFile(second->binary().path()) == "node-release-two");
#ifndef _WIN32
        REQUIRE(readFile(firstBinaryPath) == "node-release-one");
#endif

        const auto nextInstallerPath = directory.path() / "install-node.sh.next";
        writeFile(nextInstallerPath, "installer-release-two");
        replaceFile(nextInstallerPath, installerPath);
        const auto third = catalog.current();
        REQUIRE(third->binary().digest() == second->binary().digest());
        REQUIRE(readFile(third->installer().path()) == "installer-release-two");

#ifndef _WIN32
        service::node_release::Catalog expiringCatalog(std::chrono::nanoseconds::zero(),
                                                       std::chrono::nanoseconds::zero());
        const auto expiringBinaryPath = directory.path() / "expiring-node";
        const auto expiringInstallerPath = directory.path() / "expiring-install-node.sh";
        const auto expiringManifestPath = directory.path() / "expiring-node-release.manifest";
        writeFile(expiringBinaryPath, "expiring-node-release-one");
        writeFile(expiringInstallerPath, "expiring-installer-release-one");
        const auto expiringFirstDigest = flexedge::node::binarySha256(expiringBinaryPath);
        writeFile(expiringManifestPath,
                  "flexedge-node-release-v1\nversion=1.0.0\nsha256=" + expiringFirstDigest + "\n");
        expiringCatalog.configure(expiringBinaryPath, expiringInstallerPath, expiringManifestPath);
        auto expiringFirst = expiringCatalog.current();
        const auto retiredBinaryPath = expiringFirst->binary().path();
        const auto replacementPath = directory.path() / "expiring-node.next";
        writeFile(replacementPath, "expiring-node-release-two");
        replaceFile(replacementPath, expiringBinaryPath);
        const auto expiringSecondDigest = flexedge::node::binarySha256(expiringBinaryPath);
        const auto replacementManifestPath =
            directory.path() / "expiring-node-release.manifest.next";
        writeFile(replacementManifestPath,
                  "flexedge-node-release-v1\nversion=2.0.0\nsha256=" + expiringSecondDigest + "\n");
        replaceFile(replacementManifestPath, expiringManifestPath);
        const auto expiringSecond = expiringCatalog.current();
        REQUIRE(expiringSecond->binary().digest() == expiringSecondDigest);
        expiringFirst.reset();
        (void)expiringCatalog.current();
        std::ifstream retiredBinary(retiredBinaryPath, std::ios::binary);
        REQUIRE(!retiredBinary);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#undef REQUIRE
