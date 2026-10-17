#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include "common/file_digest.h"
#include "service/features/node_release/catalog.h"

#define REQUIRE(condition)                                               \
    do {                                                                 \
        if (!(condition))                                                \
            throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

class temporary_directory final {
public:
    temporary_directory()
        : path_(std::filesystem::temp_directory_path() /
                ("flexedge-node-release-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("could not write test release artifact");
    }
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read test release artifact");
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void replace_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef _WIN32
    std::filesystem::remove(destination);
#endif
    std::filesystem::rename(source, destination);
}

}  // namespace

int main() {
    try {
        temporary_directory directory;
        const auto binary_path = directory.path() / "node";
        const auto installer_path = directory.path() / "install-node.sh";
        const auto manifest_path = directory.path() / "node-release.manifest";
        write_file(binary_path, "node-release-one");
        write_file(installer_path, "installer-release-one");
        const auto first_digest = flexedge::crypto::file_sha256(binary_path);
        write_file(manifest_path,
            "flexedge-node-release-v1\r\nversion=1.0.0\r\nsha256=" + first_digest + "\r\n");

        {
            bool failed = false;
            try {
                (void)service::node_release::load(directory.path() / "missing-node", installer_path, manifest_path);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
        }

        const service::node_release::catalog catalog(
            service::node_release::load(binary_path, installer_path, manifest_path));
        const auto first = catalog.current();
        REQUIRE(first->binary().digest() == first_digest);
        REQUIRE(first->version() == "1.0.0");
        const auto first_binary_path = first->binary().path();
        const auto first_installer_path = first->installer().path();
        REQUIRE(read_file(first_binary_path) == "node-release-one");
        REQUIRE(read_file(first_installer_path) == "installer-release-one");
        REQUIRE(first->binary().size() == std::string_view("node-release-one").size());
        REQUIRE(first->binary().contents_range(0, 4) == "node");
        REQUIRE(first->binary().contents_range(4, 1024) == "-release-one");

        // The release loaded at startup stays immutable: replaced files are
        // served only after a restart loads them.
        const auto next_binary_path = directory.path() / "node.next";
        write_file(next_binary_path, "node-release-two");
        replace_file(next_binary_path, binary_path);
        const auto second_digest = flexedge::crypto::file_sha256(binary_path);
        {
            bool inconsistent = false;
            try {
                (void)service::node_release::load(binary_path, installer_path, manifest_path);
            } catch (const std::runtime_error&) {
                inconsistent = true;
            }
            REQUIRE(inconsistent);
        }
        const auto next_manifest_path = directory.path() / "node-release.manifest.next";
        write_file(next_manifest_path,
            "flexedge-node-release-v1\nversion=2.0.0\nsha256=" + second_digest + "\n");
        replace_file(next_manifest_path, manifest_path);
        REQUIRE(catalog.current() == first);
        REQUIRE(catalog.current()->version() == "1.0.0");
#ifndef _WIN32
        REQUIRE(read_file(first_binary_path) == "node-release-one");
#endif
        const auto second = service::node_release::load(binary_path, installer_path, manifest_path);
        REQUIRE(second->binary().digest() == second_digest);
        REQUIRE(second->version() == "2.0.0");
        REQUIRE(read_file(second->binary().path()) == "node-release-two");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#undef REQUIRE
