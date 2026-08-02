// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The RFC 6455 opening handshake, as a pure function of bytes.
//
// The handshake is the only part of a websocket client with a cryptographic
// check in it, and it is routinely skipped: plenty of clients send the upgrade
// request and then just look for "101" in the response. That is not what the
// check is for.
//
// `Sec-WebSocket-Accept` is base64(SHA1(client_key + GUID)). Verifying it proves
// the peer actually parsed our request and speaks the protocol, rather than
// being an intermediary that will happily return 101 and then hand us bytes
// that are not frames. Getting that wrong turns every subsequent length field
// into garbage read with pointer arithmetic, which is the failure mode
// ws_frame.hpp is written defensively against — so it is worth not reaching.
//
// SHA-1 is here because the specification names it, not because it is a
// reasonable hash in 2026. It is used for exactly one thing — proving the peer
// echoed a nonce — and nothing about that use depends on collision resistance.
// It is 60 lines and it removes the last excuse for a dependency.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace crossbook::net {

/// The magic value from RFC 6455 §1.3. Not a secret; its job is to make the
/// response impossible to produce by echoing the request.
inline constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

namespace detail {

[[nodiscard]] constexpr std::uint32_t rotl32(std::uint32_t v, int bits) noexcept {
    return (v << bits) | (v >> (32 - bits));
}

}  // namespace detail

/// SHA-1 (FIPS 180-4). Sufficient for the handshake, and used for nothing else.
[[nodiscard]] inline std::array<std::uint8_t, 20> sha1(std::string_view data) noexcept {
    std::uint32_t h[5] = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};

    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8U;

    // Message + 0x80 + zero padding to 56 mod 64 + 8-byte big-endian length.
    const std::size_t padded_len = ((data.size() + 8) / 64 + 1) * 64;

    auto byte_at = [&](std::size_t i) -> std::uint8_t {
        if (i < data.size()) {
            return static_cast<std::uint8_t>(data[i]);
        }
        if (i == data.size()) {
            return 0x80U;
        }
        if (i >= padded_len - 8) {
            const std::size_t shift = (padded_len - 1 - i) * 8;
            return static_cast<std::uint8_t>((bit_len >> shift) & 0xFFU);
        }
        return 0U;
    };

    std::array<std::uint32_t, 80> w{};
    for (std::size_t chunk = 0; chunk < padded_len; chunk += 64) {
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(byte_at(chunk + i * 4)) << 24) |
                   (static_cast<std::uint32_t>(byte_at(chunk + i * 4 + 1)) << 16) |
                   (static_cast<std::uint32_t>(byte_at(chunk + i * 4 + 2)) << 8) |
                   static_cast<std::uint32_t>(byte_at(chunk + i * 4 + 3));
        }
        for (std::size_t i = 16; i < 80; ++i) {
            w[i] = detail::rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h[0];
        std::uint32_t b = h[1];
        std::uint32_t c = h[2];
        std::uint32_t d = h[3];
        std::uint32_t e = h[4];

        for (std::size_t i = 0; i < 80; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t temp = detail::rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = detail::rotl32(b, 30);
            b = a;
            a = temp;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFFU);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFFU);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFFU);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFFU);
    }
    return out;
}

/// Standard base64 with padding.
[[nodiscard]] inline std::string base64_encode(const std::uint8_t* data, std::size_t len) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= len) {
        const std::uint32_t block = (static_cast<std::uint32_t>(data[i]) << 16) |
                                    (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                    static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kAlphabet[(block >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(block >> 12) & 0x3FU]);
        out.push_back(kAlphabet[(block >> 6) & 0x3FU]);
        out.push_back(kAlphabet[block & 0x3FU]);
        i += 3;
    }

    if (i < len) {
        const std::size_t remaining = len - i;
        std::uint32_t block = static_cast<std::uint32_t>(data[i]) << 16;
        if (remaining == 2) {
            block |= static_cast<std::uint32_t>(data[i + 1]) << 8;
        }
        out.push_back(kAlphabet[(block >> 18) & 0x3FU]);
        out.push_back(kAlphabet[(block >> 12) & 0x3FU]);
        out.push_back(remaining == 2 ? kAlphabet[(block >> 6) & 0x3FU] : '=');
        out.push_back('=');
    }
    return out;
}

/// The `Sec-WebSocket-Accept` value a conforming server must return for `key`.
[[nodiscard]] inline std::string websocket_accept_for(std::string_view key) {
    std::string combined;
    combined.reserve(key.size() + kWebSocketGuid.size());
    combined.append(key);
    combined.append(kWebSocketGuid);
    const auto digest = sha1(combined);
    return base64_encode(digest.data(), digest.size());
}

/// Build the opening handshake request.
///
/// `key` must be the base64 of 16 random bytes (§4.1). Randomness is the
/// caller's job because this header has no business owning an RNG, and because
/// a test needs to pin the key to get a reproducible request.
[[nodiscard]] inline std::string make_handshake_request(std::string_view host,
                                                        std::uint16_t port,
                                                        std::string_view path,
                                                        std::string_view key,
                                                        bool secure) {
    std::string req;
    req.reserve(256);
    req.append("GET ").append(path.empty() ? "/" : path).append(" HTTP/1.1\r\n");

    // The port is omitted when it is the scheme default: some venues route on
    // an exact Host match, and "host:443" is not the same string as "host".
    req.append("Host: ").append(host);
    const std::uint16_t default_port = secure ? 443 : 80;
    if (port != default_port) {
        req.append(":").append(std::to_string(port));
    }
    req.append("\r\n");

    req.append("Upgrade: websocket\r\n");
    req.append("Connection: Upgrade\r\n");
    req.append("Sec-WebSocket-Key: ").append(key).append("\r\n");
    req.append("Sec-WebSocket-Version: 13\r\n");
    req.append("User-Agent: crossbook/0.2\r\n");
    req.append("\r\n");
    return req;
}

enum class HandshakeStatus : std::uint8_t {
    kOk,
    /// The response headers are not complete yet; read more and retry.
    kIncomplete,
    /// Anything other than 101. The status line is worth surfacing: a 429 or a
    /// 403 from a venue is operational information, not a parse failure.
    kNotSwitchingProtocols,
    /// Upgrade / Connection headers missing or wrong.
    kNotUpgraded,
    /// Sec-WebSocket-Accept absent or did not match. See the file header for
    /// why this is checked rather than assumed.
    kBadAccept,
};

[[nodiscard]] constexpr std::string_view to_string(HandshakeStatus s) noexcept {
    switch (s) {
        case HandshakeStatus::kOk:
            return "ok";
        case HandshakeStatus::kIncomplete:
            return "incomplete";
        case HandshakeStatus::kNotSwitchingProtocols:
            return "not_switching_protocols";
        case HandshakeStatus::kNotUpgraded:
            return "not_upgraded";
        case HandshakeStatus::kBadAccept:
            return "bad_accept";
    }
    return "unknown";
}

namespace detail {

[[nodiscard]] constexpr char lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] constexpr bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] constexpr bool icontains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty() || haystack.size() < needle.size()) {
        return needle.empty();
    }
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (iequals(haystack.substr(i, needle.size()), needle)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// Look up a header, case-insensitively, in a raw header block.
[[nodiscard]] inline std::string_view find_header(std::string_view headers,
                                                  std::string_view name) noexcept {
    std::size_t pos = 0;
    while (pos < headers.size()) {
        const std::size_t eol = headers.find("\r\n", pos);
        const std::string_view line =
            headers.substr(pos, (eol == std::string_view::npos ? headers.size() : eol) - pos);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && iequals(trim(line.substr(0, colon)), name)) {
            return trim(line.substr(colon + 1));
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = eol + 2;
    }
    return {};
}

}  // namespace detail

struct HandshakeResponse {
    /// Bytes consumed by the response, so the caller knows where frames begin.
    /// A server is allowed to start sending frames in the same TCP segment as
    /// the 101, and discarding that tail loses the first message.
    std::size_t header_bytes{0};
    int status_code{0};
};

/// Validate a server's handshake response.
[[nodiscard]] inline HandshakeStatus parse_handshake_response(std::string_view response,
                                                              std::string_view expected_accept,
                                                              HandshakeResponse& out) {
    const std::size_t end = response.find("\r\n\r\n");
    if (end == std::string_view::npos) {
        return HandshakeStatus::kIncomplete;
    }
    out.header_bytes = end + 4;

    const std::size_t first_eol = response.find("\r\n");
    const std::string_view status_line = response.substr(0, first_eol);

    // "HTTP/1.1 101 Switching Protocols"
    const std::size_t sp = status_line.find(' ');
    if (sp == std::string_view::npos) {
        return HandshakeStatus::kNotSwitchingProtocols;
    }
    int code = 0;
    for (std::size_t i = sp + 1; i < status_line.size() && status_line[i] != ' '; ++i) {
        const char c = status_line[i];
        if (c < '0' || c > '9') {
            return HandshakeStatus::kNotSwitchingProtocols;
        }
        code = code * 10 + (c - '0');
    }
    out.status_code = code;
    if (code != 101) {
        return HandshakeStatus::kNotSwitchingProtocols;
    }

    const std::string_view headers = response.substr(first_eol + 2, end - first_eol - 2);

    if (!detail::iequals(detail::find_header(headers, "upgrade"), "websocket")) {
        return HandshakeStatus::kNotUpgraded;
    }
    // Connection is a comma-separated token list; "keep-alive, Upgrade" is legal.
    if (!detail::icontains(detail::find_header(headers, "connection"), "upgrade")) {
        return HandshakeStatus::kNotUpgraded;
    }
    if (detail::find_header(headers, "sec-websocket-accept") != expected_accept) {
        return HandshakeStatus::kBadAccept;
    }
    return HandshakeStatus::kOk;
}

}  // namespace crossbook::net
