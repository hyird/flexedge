#include "node/control/control_channel.h"

#include <ruvia/core/event_loop_pool.h>
#include <ruvia/core/stop_token.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include "node/control/transport_config.h"
#include "node/runtime/loop_shutdown.h"
#include "node/runtime/secure_file.h"
#include "node/runtime/state_cipher.h"
#include "node/runtime/version.h"
#include "tests/support/config_fixture.h"

#define REQUIRE(condition)                                                             \
    do {                                                                               \
        if (!(condition)) throw std::runtime_error("requirement failed: " #condition); \
    } while (false)

namespace {

constexpr std::string_view node_id = "0123456789abcdef0123456789abcdef";
constexpr std::string_view node_secret = "synthetic-node-secret-0123456789abcdef";

void append_varint(std::string& output, std::uint64_t value) {
    while (value >= 0x80) {
        output.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    output.push_back(static_cast<char>(value));
}

std::string length_delimited(std::uint32_t field, std::string_view bytes) {
    std::string result;
    append_varint(result, (static_cast<std::uint64_t>(field) << 3) | 2);
    append_varint(result, bytes.size());
    result.append(bytes);
    return result;
}

// Accepts the first TCP connection the control channel opens and closes it.
class control_listener final {
public:
    control_listener()
        : acceptor_(io_, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
          thread_([this] { serve(); }) {}

    ~control_listener() {
        stopping_ = true;
        thread_.join();
    }

    control_listener(const control_listener&) = delete;
    control_listener& operator=(const control_listener&) = delete;

    [[nodiscard]] std::uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    [[nodiscard]] bool wait_connected(std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!connected_) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

private:
    void serve() noexcept {
        std::error_code error;
        acceptor_.non_blocking(true, error);
        while (!stopping_ && !error) {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket, error);
            if (!error) {
                connected_ = true;
                return;
            }
            if (error == asio::error::would_block || error == asio::error::try_again) {
                error.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::atomic<bool> stopping_{};
    std::atomic<bool> connected_{};
    std::thread thread_;
};

struct node_start final {
    bool connected_{};
    std::int64_t applied_node_spec_revision_{};
    std::vector<std::string> state_logs_;
};

// Starts the real control channel over a state directory and reports whether it reached
// the control plane transport.
node_start start_node(const std::filesystem::path& root, const std::filesystem::path& state) {
    const auto credentials_path = root / "credentials";
    flexedge::node::write_secure_file_atomic(credentials_path,
        "node_id=" + std::string(node_id) + "\nsecret=" + std::string(node_secret) + "\n");
    auto credentials = flexedge::node::node_credentials::load(credentials_path);
    const auto binary_path = root / "node";
    flexedge::node::write_secure_file_atomic(binary_path, "synthetic node binary");

    node_start result;
    ruvia::event_loop_pool workers({.loop_count_ = 1});
    ruvia::event_loop_pool control({.loop_count_ = 1});
    flexedge::node::runtime_state runtime;
    flexedge::node::node_log_buffer logs(1);
    {
        flexedge::node::data_plane plane(workers, control.loop(0), runtime, logs);
        flexedge::node::self_updater updater({
            .binary_path_ = binary_path,
            .current_version_ = std::string(flexedge::node::node_version),
            .upgrade_record_path_ = root / "pending-upgrade",
            .node_log_ = [](std::string_view, std::string_view, std::string_view) { return true; },
            .request_restart_ = [] {},
        });
        control_listener listener;
        ruvia::stop_source stop_source;
        flexedge::node::control_channel channel(control.loop(0),
            {
                .web_socket_ = flexedge::node::control_transport_config(
                    "ws://127.0.0.1:" + std::to_string(listener.port())),
                .stop_token_ = stop_source.token(),
            },
            flexedge::node::state_store{state, credentials.secret()}, credentials, runtime, plane,
            logs, updater);
        flexedge::node::loop_shutdown shutdown(workers, control);
        workers.start();
        control.start();
        auto task = control.loop(0).start(channel.run());
        result.connected_ = listener.wait_connected(std::chrono::seconds(5));
        stop_source.request_stop();
        task.get();
        result.applied_node_spec_revision_ = runtime.applied_node_spec_revision();
        shutdown.stop_and_join();
        plane.request_stop_accepting();
    }
    if (const auto delivery = logs.take(node_id); delivery) {
        for (const auto& event : delivery->value().events()) {
            if (event.has_node_log() && event.node_log().category() == "state") {
                result.state_logs_.push_back(event.node_log().message());
            }
        }
    }
    return result;
}

// A listener-free configuration the data plane activates without binding ports.
flexedge::testing::TestConfig offline_config() {
    auto config = flexedge::testing::snapshot(false);
    config.active.mutable_node_spec()->mutable_content()->set_enabled(false);
    config.mutableWebsite()->set_https_enabled(false);
    for (auto& domain : *config.mutableWebsite()->mutable_domains()) {
        domain.set_https_enabled(false);
    }
    config.finalize();
    return config;
}

bool logged(const node_start& start, std::string_view text) {
    for (const auto& message : start.state_logs_) {
        if (message.find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

template <typename function_type>
bool rejects_as_unusable(function_type&& function) {
    try {
        function();
    } catch (const flexedge::node::stored_state_unusable&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      ("flexedge-control-channel-" +
                          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct directory_cleanup final {
        std::filesystem::path path_;
        ~directory_cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    } cleanup{root};

    try {
        // A usable persisted state is served before the control plane is reached.
        {
            const auto state = root / "valid";
            const flexedge::node::state_store store(state, node_secret);
            const auto config = offline_config();
            store.stage(config.active, config.objects);
            store.activate_staged();
            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 3);
            REQUIRE(start.state_logs_.empty());
            REQUIRE(std::filesystem::exists(state / "active" / "state.pb"));
            REQUIRE(store.load().active_.SerializeAsString() == config.active.SerializeAsString());
        }

        // An object persisted by another code line carries a field this build does not know.
        // Re-serialization moves the unknown field behind the website, so the digest differs.
        {
            const auto state = root / "unknown-field";
            const flexedge::node::state_store store(state, node_secret);
            auto config = offline_config();
            const auto content =
                length_delimited(3, "cache-policy") +
                length_delimited(10, flexedge::node::serialize_artifact(config.website()));
            const auto digest = flexedge::node::artifact_digest(std::string_view(content));
            std::vector<flexedge::node::v2::DeliveryObject> certificates;
            for (const auto& object : config.objects) {
                if (object.content().has_certificate()) {
                    certificates.push_back(object);
                }
            }
            auto* manifest = config.active.mutable_release()->mutable_content();
            for (auto& reference : *manifest->mutable_objects()) {
                if (reference.kind() == flexedge::node::v2::OBJECT_KIND_WEBSITE) {
                    reference.set_digest_sha256(digest);
                }
            }
            config.active.mutable_release()->set_digest_sha256(
                flexedge::node::artifact_digest(*manifest));
            store.stage(config.active, certificates);
            store.activate_staged();
            flexedge::node::write_secure_file_atomic(state / "objects" / (digest + ".pb"),
                flexedge::node::state_cipher(node_secret)
                    .seal(length_delimited(1, digest) + length_delimited(2, content)));
            REQUIRE(rejects_as_unusable([&] { (void)store.load_object(digest); }));
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 0);
            REQUIRE(logged(start, "failed content verification"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
            REQUIRE(std::filesystem::exists(state / "previous" / "state.pb"));
            REQUIRE(!store.load().active_.has_node_spec());

            // Only the local copy is discarded; it is fetched again under the same name.
            store.discard_object(digest);
            REQUIRE(!std::filesystem::exists(state / "objects" / (digest + ".pb")));
            REQUIRE(!store.load_object(digest));
        }

        // Tampered ciphertext no longer authenticates.
        {
            const auto state = root / "tampered";
            const flexedge::node::state_store store(state, node_secret);
            const auto config = offline_config();
            store.stage(config.active, config.objects);
            store.activate_staged();
            auto sealed = *flexedge::node::read_secure_file(state / "active" / "state.pb");
            sealed.back() = static_cast<char>(sealed.back() ^ 0x01);
            flexedge::node::write_secure_file_atomic(state / "active" / "state.pb", sealed);
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(start.applied_node_spec_revision_ == 0);
            REQUIRE(logged(start, "state.pb: stored node state authentication failed"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
            // The discarded copy replaces the previous slot instead of accumulating.
            REQUIRE(*flexedge::node::read_secure_file(state / "previous" / "state.pb") == sealed);
        }

        // Authentic bytes that are not an active state protobuf.
        {
            const auto state = root / "not-protobuf";
            const flexedge::node::state_store store(state, node_secret);
            flexedge::node::write_secure_file_atomic(state / "active" / "state.pb",
                flexedge::node::state_cipher(node_secret).seal(std::string("\xff\xff\xff", 3)));
            REQUIRE(rejects_as_unusable([&] { (void)store.load(); }));

            const auto start = start_node(root, state);
            REQUIRE(start.connected_);
            REQUIRE(logged(start, "stored active state is not valid protobuf"));
            REQUIRE(!std::filesystem::exists(state / "active" / "state.pb"));
        }

        // Control plane input keeps its checks: objects must reproduce their digest to be
        // staged, and malformed digests are not treated as a local cache problem.
        {
            const flexedge::node::state_store store(root / "boundary", node_secret);
            auto config = offline_config();
            config.objects.front().set_digest_sha256(std::string(64, 'a'));
            bool rejected = false;
            try {
                store.stage(config.active, config.objects);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            REQUIRE(rejected);
            bool invalid_digest_rejected = false;
            try {
                (void)store.load_object("not-a-digest");
            } catch (const flexedge::node::stored_state_unusable&) {
            } catch (const std::runtime_error&) {
                invalid_digest_rejected = true;
            }
            REQUIRE(invalid_digest_rejected);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
