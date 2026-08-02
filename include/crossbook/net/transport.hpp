// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Byte transport: a TCP socket, optionally wrapped in TLS.
//
// THIS IS THE ONLY PART OF CROSSBOOK THAT IS NOT HEADER-ONLY, AND THE ONLY PART
// THAT TOUCHES A PLATFORM API. That is deliberate. Everything above it — the
// framing, the decoders, the book, the verifier — is a function of bytes and
// stays testable without a network. The socket is quarantined here so that the
// interesting code does not inherit its untestability.
//
// TLS BACKENDS. `wss://` is mandatory at every venue, and there is no portable
// TLS in the standard library, so exactly one platform dependency is
// unavoidable. Rather than take a third-party one, each platform's own is used:
//
//   Windows   Schannel, which ships with the OS
//   POSIX     OpenSSL, which is present on every Linux and macOS CI image
//
// The result is that `cmake --build` produces a working client on a stock
// Windows machine with nothing installed, which is the difference between a
// reader trying the tool and a reader closing the tab. The library target
// itself remains header-only and dependency-free; this is a separate, optional
// target, and consuming crossbook as a library does not pull it in.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace crossbook::net {

enum class IoStatus : std::uint8_t {
    kOk,
    /// Nothing arrived within the timeout. Not an error: a quiet market is
    /// indistinguishable from a quiet socket, and only the caller knows which
    /// silence is acceptable.
    kTimeout,
    /// The peer closed cleanly.
    kClosed,
    /// Anything else. `last_error()` carries the platform's description.
    kError,
};

[[nodiscard]] constexpr const char* to_string(IoStatus s) noexcept {
    switch (s) {
        case IoStatus::kOk:
            return "ok";
        case IoStatus::kTimeout:
            return "timeout";
        case IoStatus::kClosed:
            return "closed";
        case IoStatus::kError:
            return "error";
    }
    return "unknown";
}

/// A bidirectional byte stream.
///
/// Blocking with timeouts rather than non-blocking with an event loop: this
/// client follows one or two sockets, and a reactor would be more machinery
/// than the problem has. The interface does not preclude one later.
class Transport {
public:
    virtual ~Transport() = default;

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /// Resolve, connect, and (for TLS) complete the handshake.
    /// `host` is also the name verified against the server certificate.
    [[nodiscard]] virtual bool connect(const std::string& host, std::uint16_t port,
                                       int timeout_ms) = 0;

    /// Read up to `len` bytes. `n_read` is set only when the status is kOk.
    ///
    /// A short read is normal and not an error — TLS records and TCP segments
    /// have nothing to do with message boundaries, which is precisely why
    /// FrameReader is incremental.
    [[nodiscard]] virtual IoStatus read(char* buf, std::size_t len, std::size_t& n_read) = 0;

    /// Write all `len` bytes, or fail. Partial writes are retried internally:
    /// a half-sent websocket frame desynchronises the stream permanently, so
    /// there is no useful way for a caller to handle one.
    [[nodiscard]] virtual IoStatus write(const char* buf, std::size_t len) = 0;

    /// Change the read timeout after connecting.
    ///
    /// The handshake and the steady state want very different numbers. An
    /// opening handshake across a congested path can legitimately take tens of
    /// seconds - measured against Kraken's edge, sometimes over twenty - while a
    /// steady-state read wants to time out in about a second so the caller's
    /// loop stays responsive to shutdown and to its own deadlines. Using one
    /// value for both means choosing between a client that gives up on a slow
    /// connect and a loop that hangs for ten seconds on every quiet market.
    ///
    /// ZERO MEANS BUSY-POLL: the socket goes non-blocking and reads return
    /// kTimeout immediately when nothing is buffered, so a caller on a
    /// dedicated core can spin instead of taking a scheduler wakeup per
    /// message. It does not mean "no timeout" — that spelling of zero is the
    /// platforms', and it is never what a latency-sensitive reader wants.
    virtual void set_read_timeout(int timeout_ms) = 0;

    /// Kernel arrival time of the most recently received data, CLOCK_REALTIME
    /// nanoseconds, or 0 where the platform offers none for TCP (Windows,
    /// macOS) or nothing has arrived. On Linux this is per socket and queried
    /// on demand, so it works identically under TLS. One read draining
    /// several coalesced segments reports the newest — callers measuring
    /// kernel-to-user delivery own that approximation.
    [[nodiscard]] virtual std::int64_t last_rx_time_ns() const noexcept { return 0; }

    virtual void close() = 0;

    [[nodiscard]] virtual bool connected() const noexcept = 0;

    /// Human-readable description of the last failure, including the platform
    /// error code. Empty when nothing has failed.
    [[nodiscard]] virtual const std::string& last_error() const noexcept = 0;

protected:
    Transport() = default;
};

/// Create a transport. `secure` selects TLS.
///
/// Returns null only if the build has no TLS backend for this platform, which
/// the CMake configuration makes an error rather than a silent downgrade —
/// falling back to plaintext against an exchange is not a graceful degradation.
[[nodiscard]] std::unique_ptr<Transport> make_transport(bool secure);

/// One-shot HTTPS GET, returning the response body.
///
/// Binance has no snapshot on its websocket stream: the documented procedure is
/// to buffer the diff stream, fetch a REST depth snapshot, and reconcile the two
/// by sequence number. Without this the Binance book cannot be started at all,
/// so a minimal HTTP client is not scope creep — it is the other half of the
/// venue's contract.
///
/// Deliberately minimal: no redirects, no chunked-encoding edge cases beyond the
/// common one, no connection reuse. It fetches one JSON document at startup.
[[nodiscard]] bool https_get(const std::string& host, const std::string& path, std::string& body,
                             std::string& error, int timeout_ms = 10'000);

}  // namespace crossbook::net
