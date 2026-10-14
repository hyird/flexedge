#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

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
        constexpr std::string_view version_alphabet =
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_+";
        for (unsigned byte = 0; byte < 256; ++byte) {
            const char character = static_cast<char>(byte);
            REQUIRE(flexedge::node::valid_node_release_version(std::string_view(&character, 1)) ==
                    (version_alphabet.find(character) != std::string_view::npos));
        }
        REQUIRE(!flexedge::node::valid_node_release_version(""));
        REQUIRE(flexedge::node::valid_node_release_version(std::string(64, 'a')));
        REQUIRE(!flexedge::node::valid_node_release_version(std::string(65, 'a')));
        REQUIRE(flexedge::node::valid_node_release_version("1.2.3-rc.1+build_7"));
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
            service::node_release::catalog concurrent_catalog(std::chrono::nanoseconds::zero());
            std::barrier start(8);
            std::atomic<unsigned> configured{}, rejected{}, unexpected{};
            std::vector<std::jthread> configurators;
            for (unsigned index = 0; index < 8; ++index) {
                configurators.emplace_back([&] {
                    start.arrive_and_wait();
                    try {
                        concurrent_catalog.configure(binary_path, installer_path, manifest_path);
                        configured.fetch_add(1);
                    } catch (const std::logic_error&) {
                        rejected.fetch_add(1);
                    } catch (...) {
                        unexpected.fetch_add(1);
                    }
                });
            }
            for (auto& configurator : configurators) {
                configurator.join();
            }
            REQUIRE(configured.load() == 1);
            REQUIRE(rejected.load() == 7);
            REQUIRE(unexpected.load() == 0);
            REQUIRE(concurrent_catalog.current()->binary().digest() == first_digest);
        }

        {
            service::node_release::catalog recovering_catalog(std::chrono::nanoseconds::zero());
            bool failed = false;
            try {
                recovering_catalog.configure(directory.path() / "missing-node", installer_path, manifest_path);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
            bool unpublished = false;
            try {
                (void)recovering_catalog.current();
            } catch (const std::logic_error&) {
                unpublished = true;
            }
            REQUIRE(unpublished);
            recovering_catalog.configure(binary_path, installer_path, manifest_path);
            REQUIRE(recovering_catalog.current()->binary().digest() == first_digest);
        }

        service::node_release::catalog catalog(std::chrono::nanoseconds::zero());
        catalog.configure(binary_path, installer_path, manifest_path);
        bool duplicate_configuration_rejected = false;
        try {
            catalog.configure(binary_path, installer_path, manifest_path);
        } catch (const std::logic_error&) {
            duplicate_configuration_rejected = true;
        }
        REQUIRE(duplicate_configuration_rejected);
        auto first = catalog.current();
        REQUIRE(first->binary().digest() == first_digest);
        REQUIRE(first->version() == "1.0.0");
        const auto first_binary_path = first->binary().path();
        const auto first_installer_path = first->installer().path();
        REQUIRE(read_file(first_binary_path) == "node-release-one");
        REQUIRE(read_file(first_installer_path) == "installer-release-one");
        REQUIRE(first->binary().size() == std::string_view("node-release-one").size());
        REQUIRE(first->binary().contents_range(0, 4) == "node");
        REQUIRE(first->binary().contents_range(4, 1024) == "-release-one");

        const auto next_binary_path = directory.path() / "node.next";
        write_file(next_binary_path, "node-release-two");
        replace_file(next_binary_path, binary_path);
        const auto stalled = catalog.current();
        REQUIRE(stalled->binary().digest() == first_digest);
        REQUIRE(stalled->version() == "1.0.0");
        const auto second_digest = flexedge::crypto::file_sha256(binary_path);
        const auto next_manifest_path = directory.path() / "node-release.manifest.next";
        write_file(next_manifest_path,
            "flexedge-node-release-v1\nversion=2.0.0\nsha256=" + second_digest + "\n");
        replace_file(next_manifest_path, manifest_path);
        first.reset();
        const auto second = catalog.current();
        REQUIRE(second->binary().digest() == second_digest);
        REQUIRE(second->version() == "2.0.0");
        REQUIRE(read_file(second->binary().path()) == "node-release-two");
#ifndef _WIN32
        REQUIRE(read_file(first_binary_path) == "node-release-one");
#endif

        const auto next_installer_path = directory.path() / "install-node.sh.next";
        write_file(next_installer_path, "installer-release-two");
        replace_file(next_installer_path, installer_path);
        const auto third = catalog.current();
        REQUIRE(third->binary().digest() == second->binary().digest());
        REQUIRE(read_file(third->installer().path()) == "installer-release-two");

#ifndef _WIN32
        service::node_release::catalog expiring_catalog(std::chrono::nanoseconds::zero(),
            std::chrono::nanoseconds::zero());
        const auto expiring_binary_path = directory.path() / "expiring-node";
        const auto expiring_installer_path = directory.path() / "expiring-install-node.sh";
        const auto expiring_manifest_path = directory.path() / "expiring-node-release.manifest";
        write_file(expiring_binary_path, "expiring-node-release-one");
        write_file(expiring_installer_path, "expiring-installer-release-one");
        const auto expiring_first_digest = flexedge::crypto::file_sha256(expiring_binary_path);
        write_file(expiring_manifest_path,
            "flexedge-node-release-v1\nversion=1.0.0\nsha256=" + expiring_first_digest + "\n");
        expiring_catalog.configure(expiring_binary_path, expiring_installer_path, expiring_manifest_path);
        auto expiring_first = expiring_catalog.current();
        const auto retired_binary_path = expiring_first->binary().path();
        const auto replacement_path = directory.path() / "expiring-node.next";
        write_file(replacement_path, "expiring-node-release-two");
        replace_file(replacement_path, expiring_binary_path);
        const auto expiring_second_digest = flexedge::crypto::file_sha256(expiring_binary_path);
        const auto replacement_manifest_path =
            directory.path() / "expiring-node-release.manifest.next";
        write_file(replacement_manifest_path,
            "flexedge-node-release-v1\nversion=2.0.0\nsha256=" + expiring_second_digest + "\n");
        replace_file(replacement_manifest_path, expiring_manifest_path);
        const auto expiring_second = expiring_catalog.current();
        REQUIRE(expiring_second->binary().digest() == expiring_second_digest);
        expiring_first.reset();
        (void)expiring_catalog.current();
        std::ifstream retired_binary(retired_binary_path, std::ios::binary);
        REQUIRE(!retired_binary);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#undef REQUIRE
