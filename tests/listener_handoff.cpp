#include "node/runtime/listener_handoff.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#if defined(__linux__)
#include <poll.h>
#include <sys/wait.h>

#include <csignal>

namespace {

using flexedge::node::consume_listener_handoff;
using flexedge::node::inherited_listeners;
using flexedge::node::owned_listener_fd;
using flexedge::node::reexec_with_listeners;

static_assert(!std::is_copy_constructible_v<owned_listener_fd>);
static_assert(!std::is_copy_assignable_v<owned_listener_fd>);
static_assert(std::is_nothrow_move_constructible_v<owned_listener_fd>);
static_assert(std::is_nothrow_move_assignable_v<owned_listener_fd>);
static_assert(!std::is_copy_constructible_v<inherited_listeners>);
static_assert(!std::is_copy_assignable_v<inherited_listeners>);
static_assert(std::is_nothrow_move_constructible_v<inherited_listeners>);

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

owned_listener_fd open_file() {
    owned_listener_fd descriptor{::open("/dev/null", O_RDONLY)};
    require(descriptor.native_handle() >= 3, "could not open ordinary test FD");
    return descriptor;
}

bool closed(int descriptor) {
    errno = 0;
    return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
}

asio::ip::tcp::endpoint endpoint_of(int descriptor) {
    asio::ip::tcp::endpoint endpoint;
    socklen_t length = static_cast<socklen_t>(endpoint.capacity());
    require(::getsockname(descriptor, endpoint.data(), &length) == 0,
        "could not obtain test socket endpoint");
    endpoint.resize(length);
    return endpoint;
}

owned_listener_fd listen_socket(const asio::ip::tcp::endpoint& endpoint) {
    owned_listener_fd descriptor{::socket(endpoint.protocol().family(), SOCK_STREAM | SOCK_CLOEXEC,
        IPPROTO_TCP)};
    require(descriptor.native_handle() >= 3, "could not create test listener");
    const int enabled = 1;
    require(::setsockopt(descriptor.native_handle(), SOL_SOCKET, SO_REUSEADDR, &enabled,
                sizeof(enabled)) == 0,
        "could not enable SO_REUSEADDR");
    require(::setsockopt(descriptor.native_handle(), SOL_SOCKET, SO_REUSEPORT, &enabled,
                sizeof(enabled)) == 0,
        "could not enable SO_REUSEPORT");
    require(::bind(descriptor.native_handle(), endpoint.data(),
                static_cast<socklen_t>(endpoint.size())) == 0,
        "could not bind test listener");
    require(::listen(descriptor.native_handle(), 16) == 0, "could not listen on test socket");
    return descriptor;
}

void publish(std::size_t workers, const std::vector<int>& descriptors) {
    std::string marker = "v1;" + std::to_string(workers) + ";";
    for (const auto descriptor : descriptors) {
        if (marker.back() != ';') {
            marker += ',';
        }
        marker += std::to_string(descriptor);
    }
    require(::setenv("FLEXEDGE_NODE_LISTEN_FDS", marker.c_str(), 1) == 0,
        "could not set test handoff marker");
}

void ownership() {
    const int number = open_file().release();
    {
        owned_listener_fd first{number};
        owned_listener_fd second{std::move(first)};
        require(first.native_handle() == -1 && second.native_handle() == number,
            "move construction did not transfer FD ownership");
        auto destination = open_file();
        const int replaced = destination.native_handle();
        destination = std::move(second);
        require(closed(replaced), "move assignment leaked its previous FD");
        require(second.native_handle() == -1 && destination.native_handle() == number,
            "move assignment did not transfer FD ownership");
        owned_listener_fd released{destination.release()};
        require(destination.native_handle() == -1, "release did not relinquish FD ownership");
        require(!closed(released.native_handle()), "release closed the transferred FD");
    }
    require(closed(number), "owner destructor did not close its FD");
    auto reused = open_file();
    require(reused.native_handle() == number, "test did not reuse the released FD number");
    {
        owned_listener_fd empty;
    }
    require(!closed(reused.native_handle()), "empty owner closed a reused FD number");

    // Keep a new FD at the old number while the moved-from owner is destroyed.
    owned_listener_fd transferred;
    owned_listener_fd replacement;
    int old_number = -1;
    {
        auto source = open_file();
        old_number = source.native_handle();
        transferred = std::move(source);
        transferred = owned_listener_fd{};
        replacement = open_file();
        require(replacement.native_handle() == old_number, "FD reuse precondition failed");
    }
    require(!closed(old_number), "moved-from destructor double-closed a reused FD number");

    {
        auto source = open_file();
        old_number = source.release();
        require(::close(old_number) == 0, "could not close explicitly released FD");
        replacement = open_file();
        require(replacement.native_handle() == old_number, "released FD reuse precondition failed");
    }
    require(!closed(old_number), "released owner double-closed a reused FD number");
}

void empty_and_rejected_handoffs() {
    require(::unsetenv("FLEXEDGE_NODE_LISTEN_FDS") == 0, "could not clear test environment");
    const asio::ip::tcp::endpoint loopback{asio::ip::address_v4::loopback(), 0};
    auto empty = consume_listener_handoff(3);
    require(empty.size() == 3, "absent handoff did not produce one bundle per worker");
    for (auto& worker : empty) {
        require(!worker.take(loopback), "absent handoff returned a listener");
        worker.close_unused();
    }

    for (const std::string kind : {"worker-count", "incomplete-group", "duplicate-number",
             "duplicate-socket", "not-listening", "malformed", "unlisted-listener",
             "stdio-number", "overflow", "trailing-delimiter"}) {
        auto listener = listen_socket(loopback);
        const int number = listener.release();
        int alias = -1;
        auto ordinary = open_file();
        if (kind == "worker-count") {
            publish(1, {number});
        } else if (kind == "duplicate-number") {
            publish(2, {number, number});
        } else if (kind == "duplicate-socket") {
            alias = ::dup(number);
            require(alias >= 3, "could not duplicate test socket");
            publish(2, {number, alias});
        } else if (kind == "not-listening") {
            publish(2, {number, ordinary.native_handle()});
        } else if (kind == "malformed") {
            require(::setenv("FLEXEDGE_NODE_LISTEN_FDS", "v1;2;broken", 1) == 0,
                "could not set malformed handoff marker");
        } else if (kind == "unlisted-listener") {
            publish(2, {});
        } else if (kind == "stdio-number") {
            publish(2, {number, 2});
        } else if (kind == "overflow") {
            require(::setenv("FLEXEDGE_NODE_LISTEN_FDS", "v1;2;999999999999999999999999999", 1) == 0,
                "could not set overflowing handoff marker");
        } else if (kind == "trailing-delimiter") {
            const auto marker = "v1;2;" + std::to_string(number) + ",";
            require(::setenv("FLEXEDGE_NODE_LISTEN_FDS", marker.c_str(), 1) == 0,
                "could not set trailing-delimiter handoff marker");
        } else {
            publish(2, {number});
        }
        bool rejected = false;
        try {
            (void)consume_listener_handoff(2);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected, "invalid handoff was not rejected");
        require(std::getenv("FLEXEDGE_NODE_LISTEN_FDS") == nullptr,
            "rejected handoff left its environment marker");
        require(closed(number), "rejected handoff stranded a listening socket");
        require(alias < 0 || closed(alias), "rejected handoff leaked an aliased socket");
        require(!closed(ordinary.native_handle()), "invalid marker closed an unrelated ordinary FD");
    }
}

void grouped_ownership() {
    const asio::ip::tcp::endpoint loopback{asio::ip::address_v4::loopback(), 0};
    auto first = listen_socket(loopback);
    const auto endpoint = endpoint_of(first.native_handle());
    auto second = listen_socket(endpoint);
    const int first_number = first.release();
    const int second_number = second.release();
    publish(2, {second_number, first_number});
    auto workers = consume_listener_handoff(2);
    require(workers.size() == 2, "handoff did not preserve worker count");
    auto left = workers[0].take(endpoint);
    require(left.has_value(), "first worker did not receive its endpoint");
    require(!workers[0].take(endpoint), "one worker received the endpoint twice");
    workers[0].close_unused();
    const int retained = left->native_handle();
    require(!closed(retained), "close_unused closed an already-taken FD");
    workers[1].close_unused();
    const int unused = retained == first_number ? second_number : first_number;
    require(closed(unused), "close_unused did not close the other worker's unused socket");
    require((::fcntl(retained, F_GETFD) & FD_CLOEXEC) != 0,
        "consumed listener was not protected from unrelated exec");
    left.reset();
    require(closed(retained), "taken listener owner leaked its socket");
}

void exec_failure_preserves_listeners() {
    auto listener = listen_socket({asio::ip::address_v4::loopback(), 0});
    const int number = listener.native_handle();
    auto ordinary = open_file();
    char name[] = "listener-handoff-test";
    char* arguments[] = {name, nullptr};
    bool failed = false;
    try {
        reexec_with_listeners("/proc/self/flexedge-no-such-upgrade", arguments, 1);
    } catch (const std::system_error& error) {
        failed = error.code().value() == ENOENT;
    }
    require(failed, "failed exec did not report its actual system error");
    require(!closed(number), "failed exec closed the rollback listener");
    require(!closed(ordinary.native_handle()), "failed exec prematurely closed an ordinary FD");
    require((::fcntl(number, F_GETFD) & FD_CLOEXEC) == 0,
        "failed exec did not leave listener ready for retry");
    require((::fcntl(ordinary.native_handle(), F_GETFD) & FD_CLOEXEC) != 0,
        "ordinary descriptor was not marked close-on-exec");
    // The same surviving FD can be handed to startup after the failed exec.
    (void)listener.release();
    auto inherited = consume_listener_handoff(1);
    require(inherited[0].take(endpoint_of(number)).has_value(),
        "failed exec lost the handoff needed by rollback");
}

void write_message(int descriptor, std::string_view message) {
    while (!message.empty()) {
        const auto written = ::send(descriptor, message.data(), message.size(), MSG_NOSIGNAL);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        require(written > 0, "could not send loopback handoff message");
        message.remove_prefix(static_cast<std::size_t>(written));
    }
}

std::string read_message(int descriptor, std::size_t size) {
    std::string result(size, '\0');
    std::size_t offset = 0;
    while (offset != size) {
        pollfd ready{descriptor, POLLIN, 0};
        int status;
        do {
            status = ::poll(&ready, 1, 10000);
        } while (status < 0 && errno == EINTR);
        require(status == 1, "timed out waiting for loopback handoff data");
        const auto received = ::recv(descriptor, result.data() + offset, size - offset, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        require(received > 0, "loopback handoff connection closed before its message");
        offset += static_cast<std::size_t>(received);
    }
    return result;
}

owned_listener_fd connect_before_exec(const asio::ip::tcp::endpoint& endpoint) {
    owned_listener_fd client{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    require(client.native_handle() >= 3, "could not create backlog client");
    require(::connect(client.native_handle(), endpoint.data(),
                static_cast<socklen_t>(endpoint.size())) == 0,
        "could not queue connection before exec");
    write_message(client.native_handle(), "queued-before-exec");
    return client;
}

int accept_child(int argc, char* argv[]) {
    require(argc == 10, "invalid exec child arguments");
    ::alarm(15);
    for (int index = 2; index < 8; ++index) {
        require(closed(std::stoi(argv[index])), "ordinary non-stdio FD leaked through exec");
    }
    require(std::getenv("FLEXEDGE_NODE_LISTEN_FDS") != nullptr,
        "exec did not carry listener handoff marker");
    auto workers = consume_listener_handoff(2);
    require(std::getenv("FLEXEDGE_NODE_LISTEN_FDS") == nullptr,
        "startup did not consume handoff marker");
    std::vector<owned_listener_fd> listeners;
    std::vector<pollfd> ready;
    for (int port_argument = 8; port_argument < 10; ++port_argument) {
        const asio::ip::tcp::endpoint endpoint{asio::ip::address_v4::loopback(),
            static_cast<unsigned short>(std::stoi(argv[port_argument]))};
        std::array<ino_t, 2> identities{};
        for (std::size_t worker = 0; worker < workers.size(); ++worker) {
            auto listener = workers[worker].take(endpoint);
            require(listener.has_value(), "exec lost an endpoint's per-worker listening socket");
            require(!workers[worker].take(endpoint), "exec assigned a socket to one worker twice");
            struct stat status{};
            require(::fstat(listener->native_handle(), &status) == 0,
                "could not inspect inherited socket");
            identities[worker] = status.st_ino;
            ready.push_back({listener->native_handle(), POLLIN, 0});
            listeners.push_back(std::move(*listener));
        }
        require(identities[0] != identities[1], "workers received aliases, not independent sockets");
    }
    // Both connections and their request bytes were queued before this image
    // existed. Accept the original sockets, not a rebound port or mock stream.
    for (int endpoint = 0; endpoint < 2; ++endpoint) {
        auto* group = ready.data() + endpoint * 2;
        int status;
        do {
            status = ::poll(group, 2, 10000);
        } while (status < 0 && errno == EINTR);
        require(status > 0, "pre-exec connection disappeared from listening backlog");
        const int index = (group[0].revents & POLLIN) != 0 ? 0 : 1;
        owned_listener_fd accepted{::accept4(group[index].fd, nullptr, nullptr, SOCK_CLOEXEC)};
        require(accepted.native_handle() >= 3, "could not accept pre-exec backlog connection");
        require(read_message(accepted.native_handle(), 18) == "queued-before-exec",
            "pre-exec backlog request bytes were lost");
        write_message(accepted.native_handle(), "accepted-after-exec");
    }
    return 0;
}

void backlog_survives_exec() {
    const asio::ip::tcp::endpoint loopback{asio::ip::address_v4::loopback(), 0};
    std::vector<owned_listener_fd> listeners;
    std::array<asio::ip::tcp::endpoint, 2> endpoints;
    for (auto& endpoint : endpoints) {
        auto first = listen_socket(loopback);
        endpoint = endpoint_of(first.native_handle());
        listeners.push_back(std::move(first));
        listeners.push_back(listen_socket(endpoint));
    }
    std::array<owned_listener_fd, 2> clients{
        connect_before_exec(endpoints[0]), connect_before_exec(endpoints[1])};
    std::array<owned_listener_fd, 2> ordinary{open_file(), open_file()};
    int pipe_descriptors[2];
    require(::pipe(pipe_descriptors) == 0, "could not create non-CLOEXEC test pipe");
    std::array<owned_listener_fd, 2> pipe{
        owned_listener_fd{pipe_descriptors[0]}, owned_listener_fd{pipe_descriptors[1]}};
    std::array<std::string, 10> arguments{
        "listener-handoff-test", "--accept-child",
        std::to_string(ordinary[0].native_handle()), std::to_string(ordinary[1].native_handle()),
        std::to_string(pipe[0].native_handle()), std::to_string(pipe[1].native_handle()),
        std::to_string(clients[0].native_handle()), std::to_string(clients[1].native_handle()),
        std::to_string(endpoints[0].port()), std::to_string(endpoints[1].port())};
    std::array<char*, 11> argv{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        argv[index] = arguments[index].data();
    }
    const auto child = ::fork();
    require(child >= 0, "could not fork exec handoff test");
    if (child == 0) {
        try {
            reexec_with_listeners("/proc/self/exe", argv.data(), 2);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            std::_Exit(1);
        }
    }
    struct child_guard {
        pid_t child_;
        ~child_guard() {
            if (child_ > 0) {
                ::kill(child_, SIGKILL);
                while (::waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {
                }
            }
        }
    } cleanup{child};
    listeners.clear();
    for (auto& client : clients) {
        require(read_message(client.native_handle(), 19) == "accepted-after-exec",
            "new executable did not serve the pre-exec backlog connection");
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    require(waited == child, "could not reap exec handoff child");
    cleanup.child_ = -1;
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "exec handoff child failed");
}

}  // namespace
#endif

int main(int argc, char* argv[]) {
    try {
#if defined(__linux__)
        if (argc > 1 && std::string_view(argv[1]) == "--accept-child") {
            return accept_child(argc, argv);
        }
        ownership();
        empty_and_rejected_handoffs();
        grouped_ownership();
        exec_failure_preserves_listeners();
        backlog_survives_exec();
#else
        (void)argc;
        (void)argv;
#endif
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
