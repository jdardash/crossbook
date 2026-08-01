// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The opening handshake, checked against published vectors.
//
// SHA-1 and base64 are implemented here rather than depended on, so they are
// tested against the specifications' own answers rather than against
// themselves. A hand-rolled hash that is only checked by round-tripping through
// itself is not checked at all.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "crossbook/net/handshake.hpp"

using namespace crossbook::net;

namespace {

std::string hex(const std::array<std::uint8_t, 20>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (const std::uint8_t byte : digest) {
        out.push_back(kHex[(byte >> 4) & 0x0FU]);
        out.push_back(kHex[byte & 0x0FU]);
    }
    return out;
}

std::string b64(std::string_view text) {
    return base64_encode(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

}  // namespace

TEST_CASE("SHA-1 matches the FIPS 180-4 vectors") {
    CHECK(hex(sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(hex(sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(hex(sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

    // A million 'a' characters: the vector that catches a broken length field,
    // since the bit count no longer fits in the low word.
    CHECK(hex(sha1(std::string(1'000'000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("base64 matches the RFC 4648 vectors") {
    CHECK(b64("") == "");
    CHECK(b64("f") == "Zg==");
    CHECK(b64("fo") == "Zm8=");
    CHECK(b64("foo") == "Zm9v");
    CHECK(b64("foob") == "Zm9vYg==");
    CHECK(b64("fooba") == "Zm9vYmE=");
    CHECK(b64("foobar") == "Zm9vYmFy");
}

TEST_CASE("The accept value matches RFC 6455's own example") {
    // Section 1.3, verbatim. This single vector is what proves the GUID, the
    // hash, and the encoding are all being combined the way a server will.
    CHECK(websocket_accept_for("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST_CASE("The request carries the headers a server requires") {
    const std::string request =
        make_handshake_request("ws.kraken.com", 443, "/v2", "dGhlIHNhbXBsZSBub25jZQ==", true);

    CHECK(request.starts_with("GET /v2 HTTP/1.1\r\n"));
    CHECK(request.find("Host: ws.kraken.com\r\n") != std::string::npos);
    CHECK(request.find("Upgrade: websocket\r\n") != std::string::npos);
    CHECK(request.find("Connection: Upgrade\r\n") != std::string::npos);
    CHECK(request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
    CHECK(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
    CHECK(request.ends_with("\r\n\r\n"));
}

TEST_CASE("The Host header omits the port only when it is the scheme default") {
    CHECK(make_handshake_request("example.com", 443, "/", "k", true).find("Host: example.com\r\n") !=
          std::string::npos);
    CHECK(make_handshake_request("example.com", 9443, "/", "k", true)
              .find("Host: example.com:9443\r\n") != std::string::npos);
    CHECK(make_handshake_request("example.com", 80, "/", "k", false).find("Host: example.com\r\n") !=
          std::string::npos);
}

TEST_CASE("A conforming response is accepted") {
    const std::string accept = websocket_accept_for("dGhlIHNhbXBsZSBub25jZQ==");
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";

    HandshakeResponse parsed;
    REQUIRE(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kOk);
    CHECK(parsed.status_code == 101);
    CHECK(parsed.header_bytes == response.size());
}

TEST_CASE("Frame bytes packed behind the 101 are reported, not lost") {
    // A server is entitled to put the first frames in the same segment as the
    // handshake response. header_bytes is what tells the caller where they
    // start; getting it wrong drops the first message intermittently.
    const std::string accept = websocket_accept_for("abc");
    const std::string headers =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";
    const std::string response = headers + "\x81\x02hi";

    HandshakeResponse parsed;
    REQUIRE(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kOk);
    CHECK(parsed.header_bytes == headers.size());
    CHECK(response.size() - parsed.header_bytes == 4);
}

TEST_CASE("Header matching is case-insensitive and tolerates token lists") {
    const std::string accept = websocket_accept_for("abc");
    const std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "upgrade: WebSocket\r\n"
        "connection: keep-alive, Upgrade\r\n"
        "sec-websocket-accept: " +
        accept + "\r\n\r\n";

    HandshakeResponse parsed;
    CHECK(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kOk);
}

TEST_CASE("Responses that are not a valid upgrade are rejected") {
    const std::string accept = websocket_accept_for("abc");
    HandshakeResponse parsed;

    SECTION("incomplete headers ask for more bytes") {
        CHECK(parse_handshake_response("HTTP/1.1 101 Switching Protocols\r\nUpgrade: web", accept,
                                       parsed) == HandshakeStatus::kIncomplete);
    }
    SECTION("a non-101 status is surfaced with its code") {
        CHECK(parse_handshake_response("HTTP/1.1 429 Too Many Requests\r\n\r\n", accept, parsed) ==
              HandshakeStatus::kNotSwitchingProtocols);
        CHECK(parsed.status_code == 429);
    }
    SECTION("a missing Upgrade header is rejected") {
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
            accept + "\r\n\r\n";
        CHECK(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kNotUpgraded);
    }
    SECTION("a wrong accept value is rejected") {
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: c3VyZWx5IG5vdA==\r\n\r\n";
        CHECK(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kBadAccept);
    }
    SECTION("a missing accept header is rejected") {
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
        CHECK(parse_handshake_response(response, accept, parsed) == HandshakeStatus::kBadAccept);
    }
}
