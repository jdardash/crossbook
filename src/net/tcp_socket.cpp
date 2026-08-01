// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include "tcp_socket.hpp"

#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // FormatMessageA / LocalFree; must follow winsock2.h.
// clang-format on
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// Linux suppresses SIGPIPE per-send with MSG_NOSIGNAL; macOS and the BSDs do it
// per-socket with SO_NOSIGPIPE and do not define the flag at all. Without one of
// the two, a venue closing the connection mid-write kills the process outright
// rather than returning an error the caller can act on.
#ifndef MSG_NOSIGNAL
#define CROSSBOOK_MSG_NOSIGNAL 0
#else
#define CROSSBOOK_MSG_NOSIGNAL MSG_NOSIGNAL
#endif
#endif

namespace crossbook::net::detail {
namespace {

#ifdef _WIN32
/// WSAStartup exactly once, without a static initialisation order problem.
///
/// Function-local static initialisation is thread-safe since C++11, which is
/// what makes this correct in the presence of two feeds connecting at once.
struct WinsockInit {
    WinsockInit() {
        WSADATA data{};
        result = ::WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockInit() {
        if (result == 0) {
            ::WSACleanup();
        }
    }
    WinsockInit(const WinsockInit&) = delete;
    WinsockInit& operator=(const WinsockInit&) = delete;
    int result{0};
};

bool ensure_winsock() {
    static WinsockInit init;
    return init.result == 0;
}

int last_socket_error() { return ::WSAGetLastError(); }
#else
int last_socket_error() { return errno; }
#endif

}  // namespace

std::string socket_error_string(const char* context) {
    const int code = last_socket_error();
    std::string out(context);
    out.append(": ");

#ifdef _WIN32
    char* message = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<char*>(&message), 0, nullptr);
    if (len != 0 && message != nullptr) {
        std::string text(message, len);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        out.append(text);
        ::LocalFree(message);
    } else {
        out.append("unknown error");
    }
#else
    out.append(std::strerror(code));
#endif

    out.append(" (").append(std::to_string(code)).append(")");
    return out;
}

bool TcpSocket::connect(const std::string& host, std::uint16_t port, int timeout_ms,
                        std::string& error) {
#ifdef _WIN32
    if (!ensure_winsock()) {
        error = socket_error_string("WSAStartup");
        return false;
    }
#endif

    close();

    // Strip the brackets from an IPv6 literal: they belong to the URL syntax,
    // not to the address.
    std::string node = host;
    if (node.size() >= 2 && node.front() == '[' && node.back() == ']') {
        node = node.substr(1, node.size() - 2);
    }

    ::addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // v4 or v6, whichever resolves.
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ::addrinfo* results = nullptr;
    const std::string service = std::to_string(port);
    const int rc = ::getaddrinfo(node.c_str(), service.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
#ifdef _WIN32
        error = socket_error_string("getaddrinfo");
#else
        error = std::string("getaddrinfo: ") + ::gai_strerror(rc);
#endif
        return false;
    }

    std::string last_failure;
    for (::addrinfo* it = results; it != nullptr; it = it->ai_next) {
        const SocketHandle fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kInvalidSocket) {
            last_failure = socket_error_string("socket");
            continue;
        }

        if (::connect(fd, it->ai_addr, static_cast<SockLen>(it->ai_addrlen)) != 0) {
            last_failure = socket_error_string("connect");
#ifdef _WIN32
            ::closesocket(fd);
#else
            ::close(fd);
#endif
            continue;
        }

        fd_ = fd;
        break;
    }
    ::freeaddrinfo(results);

    if (fd_ == kInvalidSocket) {
        error = last_failure.empty() ? "connect: no address succeeded" : last_failure;
        return false;
    }

    // Nagle batches small writes, which for a subscribe frame means waiting for
    // an ACK before the exchange even sees it. The payloads here are tiny and
    // latency-relevant; there is nothing to coalesce.
    int one = 1;
    (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one),
                       static_cast<SockLen>(sizeof(one)));

#if defined(SO_NOSIGPIPE)
    // The macOS / BSD half of the SIGPIPE story; see the note by the include.
    (void)::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, static_cast<SockLen>(sizeof(one)));
#endif

    set_read_timeout(timeout_ms);

    error.clear();
    return true;
}

void TcpSocket::set_read_timeout(int timeout_ms) noexcept {
    if (fd_ == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    // Windows takes the timeout as a DWORD of milliseconds.
    auto ms = static_cast<DWORD>(timeout_ms);
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms),
                       static_cast<SockLen>(sizeof(ms)));
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms),
                       static_cast<SockLen>(sizeof(ms)));
#else
    ::timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout_ms % 1000) * 1000);
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, static_cast<SockLen>(sizeof(tv)));
    (void)::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, static_cast<SockLen>(sizeof(tv)));
#endif
}

IoStatus TcpSocket::read(char* buf, std::size_t len, std::size_t& n_read, std::string& error) {
    if (fd_ == kInvalidSocket) {
        error = "read: socket not connected";
        return IoStatus::kError;
    }

#ifdef _WIN32
    const int got = ::recv(fd_, buf, static_cast<int>(len), 0);
#else
    const auto got = ::recv(fd_, buf, len, 0);
#endif

    if (got > 0) {
        n_read = static_cast<std::size_t>(got);
        return IoStatus::kOk;
    }
    if (got == 0) {
        return IoStatus::kClosed;  // Orderly shutdown by the peer.
    }

#ifdef _WIN32
    const int code = ::WSAGetLastError();
    if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
        return IoStatus::kTimeout;
    }
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return IoStatus::kTimeout;
    }
    if (errno == EINTR) {
        // A signal, not a failure. Report it as a timeout so the caller's loop
        // simply comes round again.
        return IoStatus::kTimeout;
    }
#endif

    error = socket_error_string("recv");
    return IoStatus::kError;
}

IoStatus TcpSocket::write(const char* buf, std::size_t len, std::string& error) {
    if (fd_ == kInvalidSocket) {
        error = "write: socket not connected";
        return IoStatus::kError;
    }

    std::size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        const int n = ::send(fd_, buf + sent, static_cast<int>(len - sent), 0);
#else
        const auto n = ::send(fd_, buf + sent, len - sent, CROSSBOOK_MSG_NOSIGNAL);
#endif
        if (n > 0) {
            sent += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) {
            return IoStatus::kClosed;
        }

#ifdef _WIN32
        const int code = ::WSAGetLastError();
        if (code == WSAETIMEDOUT || code == WSAEWOULDBLOCK) {
            return IoStatus::kTimeout;
        }
#else
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return IoStatus::kTimeout;
        }
#endif
        error = socket_error_string("send");
        return IoStatus::kError;
    }
    return IoStatus::kOk;
}

void TcpSocket::close() noexcept {
    if (fd_ == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    ::closesocket(fd_);
#else
    ::close(fd_);
#endif
    fd_ = kInvalidSocket;
}

}  // namespace crossbook::net::detail
