// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The socket, over loopback.
//
// The transport was the one layer with no tests at all: everything above it is
// a function of bytes, and the socket was quarantined precisely so the
// interesting code would not inherit its untestability. But the busy-poll
// contract — a zero read timeout means "return immediately", not "block
// forever" — is a behavioural cliff worth pinning: both platforms' native
// meaning of a zero SO_RCVTIMEO is the exact opposite.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "tcp_socket.hpp"

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace crossbook::net;

namespace {

/// A one-connection loopback listener, raw enough to not depend on the code
/// under test.
class LoopbackListener {
public:
    LoopbackListener() {
#ifdef _WIN32
        WSADATA wsa{};
        (void)::WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        ::sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;  // Any free port.
        (void)::bind(listen_fd_, reinterpret_cast<::sockaddr*>(&addr),
                     static_cast<detail::SockLen>(sizeof(addr)));
        (void)::listen(listen_fd_, 1);
        detail::SockLen len = static_cast<detail::SockLen>(sizeof(addr));
        (void)::getsockname(listen_fd_, reinterpret_cast<::sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
    }

    ~LoopbackListener() {
        close_peer();
        if (listen_fd_ != detail::kInvalidSocket) {
#ifdef _WIN32
            ::closesocket(listen_fd_);
#else
            ::close(listen_fd_);
#endif
        }
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

    /// Accept the pending connection. Call after the client's connect.
    void accept_peer() { peer_fd_ = ::accept(listen_fd_, nullptr, nullptr); }

    void send_bytes(const char* data, std::size_t len) {
#ifdef _WIN32
        (void)::send(peer_fd_, data, static_cast<int>(len), 0);
#else
        (void)::send(peer_fd_, data, len, 0);
#endif
    }

    void close_peer() {
        if (peer_fd_ != detail::kInvalidSocket) {
#ifdef _WIN32
            ::closesocket(peer_fd_);
#else
            ::close(peer_fd_);
#endif
            peer_fd_ = detail::kInvalidSocket;
        }
    }

private:
    detail::SocketHandle listen_fd_{detail::kInvalidSocket};
    detail::SocketHandle peer_fd_{detail::kInvalidSocket};
    std::uint16_t port_{0};
};

}  // namespace

TEST_CASE("a zero read timeout busy-polls instead of blocking", "[transport]") {
    LoopbackListener listener;
    REQUIRE(listener.port() != 0);

    detail::TcpSocket sock;
    std::string error;
    REQUIRE(sock.connect("127.0.0.1", listener.port(), 2'000, error));
    listener.accept_peer();

    sock.set_read_timeout(0);

    // Nothing has been sent: the read must come back kTimeout immediately.
    // Under the platforms' native meaning of a zero timeout this call would
    // block forever and this test would hang rather than fail, which is why
    // the elapsed-time bound is generous but real.
    char buf[16];
    std::size_t got = 0;
    const auto before = std::chrono::steady_clock::now();
    const IoStatus empty = sock.read(buf, sizeof(buf), got, error);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    CHECK(empty == IoStatus::kTimeout);
    CHECK(elapsed < std::chrono::milliseconds(200));

    // Data pushed from the far side is picked up by the spin.
    listener.send_bytes("hello", 5);
    IoStatus status = IoStatus::kTimeout;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (status == IoStatus::kTimeout && std::chrono::steady_clock::now() < deadline) {
        status = sock.read(buf, sizeof(buf), got, error);
    }
    REQUIRE(status == IoStatus::kOk);
    CHECK(got == 5);
    CHECK(std::memcmp(buf, "hello", 5) == 0);
}

TEST_CASE("a nonzero timeout still blocks and still delivers", "[transport]") {
    // The busy-poll contract must not have broken the ordinary path.
    LoopbackListener listener;
    detail::TcpSocket sock;
    std::string error;
    REQUIRE(sock.connect("127.0.0.1", listener.port(), 2'000, error));
    listener.accept_peer();

    sock.set_read_timeout(50);
    char buf[16];
    std::size_t got = 0;
    CHECK(sock.read(buf, sizeof(buf), got, error) == IoStatus::kTimeout);

    listener.send_bytes("book", 4);
    sock.set_read_timeout(2'000);
    REQUIRE(sock.read(buf, sizeof(buf), got, error) == IoStatus::kOk);
    CHECK(got == 4);
}

TEST_CASE("the kernel receive timestamp is exposed where the platform has one",
          "[transport]") {
    LoopbackListener listener;
    detail::TcpSocket sock;
    std::string error;
    REQUIRE(sock.connect("127.0.0.1", listener.port(), 2'000, error));
    listener.accept_peer();

    // Before any data: no timestamp to report.
    CHECK(sock.last_rx_time_ns() == 0);

#ifdef __linux__
    // The kernel arms packet stamping through a deferred static key, so a
    // segment sent immediately after connect can legitimately arrive without a
    // stamp. Real captures run for seconds and never notice; a test that
    // sends within microseconds of connect must wait the key out.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
#endif
    listener.send_bytes("stamp", 5);
    sock.set_read_timeout(2'000);
    char buf[16];
    std::size_t got = 0;
    REQUIRE(sock.read(buf, sizeof(buf), got, error) == IoStatus::kOk);

#ifdef __linux__
    // Linux reports the kernel's arrival time for the last delivered segment.
    // It is a CLOCK_REALTIME value; sanity-bound it against the same clock.
    const std::int64_t rx = sock.last_rx_time_ns();
    REQUIRE(rx != 0);
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    CHECK(now - rx < 5'000'000'000);  // Within 5 s of now.
    CHECK(now - rx > -1'000'000'000);  // And not from the future.
#else
    // Windows and macOS have no per-segment receive timestamp for TCP that is
    // worth pretending about; the accessor reports 0 and the measurement
    // starts in user space, as documented.
    CHECK(sock.last_rx_time_ns() == 0);
#endif
}
