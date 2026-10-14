#include "node/runtime/secure_file.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)
int main() {
    try {
        const auto state_directory =
            std::filesystem::temp_directory_path() /
            ("flexedge-node-state-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct state_directory_cleanup final {
            std::filesystem::path path_;
            ~state_directory_cleanup() {
                std::error_code ignored;
                std::filesystem::remove_all(path_, ignored);
            }
        } state_cleanup{state_directory};
        {
            const auto target = state_directory / "replacement-directory";
            std::filesystem::create_directories(target);
            bool failed = false;
            try {
                flexedge::node::write_secure_file_atomic(target, "new state");
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
            REQUIRE(std::filesystem::is_directory(target));
            REQUIRE(!std::filesystem::exists(target.string() + ".tmp"));
            failed = false;
            try {
                (void)flexedge::node::read_secure_file(target);
            } catch (const std::runtime_error&) {
                failed = true;
            }
            REQUIRE(failed);
            const auto regular = state_directory / "read-roundtrip";
            REQUIRE(!flexedge::node::read_secure_file(regular).has_value());
            std::string content(32 * 1024 + 1, 'x');
            content[16 * 1024] = '\0';
            flexedge::node::write_secure_file_atomic(regular, content);
            REQUIRE(flexedge::node::read_secure_file(regular) == content);
            flexedge::node::write_secure_file_atomic(regular, "short");
            REQUIRE(flexedge::node::read_secure_file(regular) == "short");
            flexedge::node::write_secure_file_atomic(regular, "");
            const auto empty = flexedge::node::read_secure_file(regular);
            REQUIRE(empty.has_value() && empty->empty());
            REQUIRE(!std::filesystem::exists(regular.string() + ".tmp"));
            std::atomic<int> successful_writes{0};
            std::atomic<bool> unexpected_failure{false};
            std::vector<std::thread> writers;
            for (int i = 0; i < 8; ++i) {
                writers.emplace_back([&, i] {
                    try {
                        flexedge::node::write_secure_file_atomic(
                            regular, std::string(32768, static_cast<char>('a' + i)));
                        ++successful_writes;
                    } catch (const std::runtime_error&) {
                        // Windows may reject competing replacement operations.
                    } catch (...) {
                        unexpected_failure = true;
                    }
                });
            }
            for (auto& writer : writers) {
                writer.join();
            }
            REQUIRE(successful_writes > 0);
            REQUIRE(!unexpected_failure);
#ifndef _WIN32
            REQUIRE(successful_writes == 8);
#endif
            const auto concurrent = flexedge::node::read_secure_file(regular);
            REQUIRE(concurrent && concurrent->size() == 32768);
            REQUIRE(*concurrent == std::string(32768, concurrent->front()));
            for (const auto& entry : std::filesystem::directory_iterator(state_directory)) {
                REQUIRE(entry.path().filename().string().find(".tmp.") == std::string::npos);
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
