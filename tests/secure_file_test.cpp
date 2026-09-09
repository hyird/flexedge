#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <atomic>
#include "node/runtime/secure_file.h"
#define REQUIRE(condition) do { if (!(condition)) throw std::runtime_error("requirement failed: " #condition); } while(false)
int main() {
    try {
        const auto stateDirectory =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-state-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct StateDirectoryCleanup final {
            std::filesystem::path path;
            ~StateDirectoryCleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path, ignored);
            }
        } stateCleanup{stateDirectory};
        {
            const auto target = stateDirectory / "replacement-directory";
            std::filesystem::create_directories(target);
            bool failed = false;
            try {
                flexedge::node::writeSecureFileAtomic(target, "new state");
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
            REQUIRE(std::filesystem::is_directory(target));
            REQUIRE(!std::filesystem::exists(target.string() + ".tmp"));
            failed = false;
            try {
                (void)flexedge::node::readSecureFile(target);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
            const auto regular = stateDirectory / "read-roundtrip";
            REQUIRE(!flexedge::node::readSecureFile(regular).has_value());
            std::string content(32 * 1024 + 1, 'x');
            content[16 * 1024] = '\0';
            flexedge::node::writeSecureFileAtomic(regular, content);
            REQUIRE(flexedge::node::readSecureFile(regular) == content);
            flexedge::node::writeSecureFileAtomic(regular, "short");
            REQUIRE(flexedge::node::readSecureFile(regular) == "short");
            flexedge::node::writeSecureFileAtomic(regular, "");
            const auto empty = flexedge::node::readSecureFile(regular);
            REQUIRE(empty.has_value() && empty->empty());
            REQUIRE(!std::filesystem::exists(regular.string() + ".tmp"));
            std::atomic<int> successfulWrites{0};
            std::atomic<bool> unexpectedFailure{false};
            std::vector<std::thread> writers;
            for (int i = 0; i < 8; ++i) {
                writers.emplace_back([&, i] {
                    try {
                        flexedge::node::writeSecureFileAtomic(
                            regular, std::string(32768, static_cast<char>('a' + i)));
                        ++successfulWrites;
                    } catch (const std::runtime_error&) {
                        // Windows may reject competing replacement operations.
                    } catch (...) {
                        unexpectedFailure = true;
                    }
                });
            }
            for (auto& writer : writers) writer.join();
            REQUIRE(successfulWrites > 0);
            REQUIRE(!unexpectedFailure);
#ifndef _WIN32
            REQUIRE(successfulWrites == 8);
#endif
            const auto concurrent = flexedge::node::readSecureFile(regular);
            REQUIRE(concurrent && concurrent->size() == 32768);
            REQUIRE(*concurrent == std::string(32768, concurrent->front()));
            for (const auto& entry : std::filesystem::directory_iterator(stateDirectory)) {
                REQUIRE(entry.path().filename().string().find(".tmp.") == std::string::npos);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
