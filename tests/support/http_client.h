#pragma once
#include <string>
#include <string_view>
#include <array>
#include <chrono>
#include <thread>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>
namespace flexedge::testing {
inline std::string request(std::uint16_t port, std::string_view host = "WWW.Example.COM:80",
                    std::string_view extraHeaders = {}, std::string_view target = "/a?q=1",
                    std::string_view method = "GET", std::string_view body = {}) {
    asio::io_context context;
    asio::ip::tcp::socket socket(context);
    socket.connect({asio::ip::address_v4::loopback(), port});
    const auto bytes = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: " + std::string(host) + "\r\n" +
                       std::string(extraHeaders) + (body.empty() ? "" : "Content-Length: " + std::to_string(body.size()) + "\r\n") +
                       "Connection: close\r\n\r\n" + std::string(body);
    asio::write(socket, asio::buffer(bytes));
    std::string response;
    std::array<char, 1024> buffer{};
    std::error_code error;
    for (;;) {
        const auto size = socket.read_some(asio::buffer(buffer), error);
        response.append(buffer.data(), size);
        if (error == asio::error::eof) {
            break;
        }
        if (error) {
            throw std::system_error(error, "could not read edge response");
        }
    }
    return response;
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}


}
