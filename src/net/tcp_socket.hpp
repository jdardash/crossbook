// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Internal: a blocking TCP socket with timeouts, shared by the plaintext
// transport and by both TLS backends (each of which needs somewhere to put the
// bytes its handshake produces).
//
// Not installed and not part of the public interface. Nothing above the
// transport layer should know what a socket is.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "crossbook/net/transport.hpp"

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#endif

namespace crossbook::net::detail {

#ifdef _WIN32
using SocketHandle = ::SOCKET;
inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

/// Format a platform socket error as "message (code)".
[[nodiscard]] std::string socket_error_string(const char* context);

/// Blocking TCP socket with send and receive timeouts.
class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close(); }

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// Resolve `host` and connect to the first address that accepts.
    ///
    /// Every result from getaddrinfo is tried in turn, so a host that publishes
    /// an AAAA record on a machine with no IPv6 route still connects rather than
    /// failing on the first candidate.
    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port, int timeout_ms,
                               std::string& error);

    [[nodiscard]] IoStatus read(char* buf, std::size_t len, std::size_t& n_read,
                                std::string& error);

    /// Write all of `len`, looping over partial sends.
    [[nodiscard]] IoStatus write(const char* buf, std::size_t len, std::string& error);

    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept { return fd_ != kInvalidSocket; }
    [[nodiscard]] SocketHandle handle() const noexcept { return fd_; }

private:
    SocketHandle fd_{kInvalidSocket};
};

}  // namespace crossbook::net::detail
