// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "crossbook/websocket.hpp"

using namespace crossbook;
using namespace crossbook::ws;

namespace {

/// Build an unmasked server-to-client frame by hand, so the tests exercise the
/// decoder against bytes rather than against the encoder.
std::string server_frame(Opcode opcode, std::string_view payload, bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode)));
    const std::size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<std::uint64_t>(n) >> (i * 8)) & 0xFF));
        }
    }
    out.append(payload);
    return out;
}

}  // namespace

TEST_CASE("a short text frame decodes", "[ws]") {
    const std::string bytes = server_frame(Opcode::kText, "hello");
    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);
    CHECK(frame.opcode == Opcode::kText);
    CHECK(frame.fin);
    CHECK(frame.payload == "hello");
    CHECK(consumed == bytes.size());
}

TEST_CASE("an empty payload is valid", "[ws]") {
    const std::string bytes = server_frame(Opcode::kText, "");
    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);
    CHECK(frame.payload.empty());
    CHECK(consumed == 2);
}

TEST_CASE("extended lengths decode", "[ws]") {
    SECTION("16-bit") {
        const std::string payload(1000, 'x');
        const std::string bytes = server_frame(Opcode::kBinary, payload);
        Frame frame{};
        std::size_t consumed = 0;
        REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);
        CHECK(frame.payload.size() == 1000);
        CHECK(consumed == bytes.size());
    }
    SECTION("64-bit") {
        const std::string payload(70'000, 'y');
        const std::string bytes = server_frame(Opcode::kBinary, payload);
        Frame frame{};
        std::size_t consumed = 0;
        REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);
        CHECK(frame.payload.size() == 70'000);
    }
}

TEST_CASE("a partial frame reports incomplete without consuming", "[ws]") {
    // The normal state of a stream, and not an error. Consuming here would
    // desynchronise the connection permanently.
    const std::string bytes = server_frame(Opcode::kText, "hello world");
    for (std::size_t prefix = 0; prefix < bytes.size(); ++prefix) {
        INFO("prefix length " << prefix);
        Frame frame{};
        std::size_t consumed = 999;
        CHECK(decode_frame(std::string_view(bytes).substr(0, prefix), frame, consumed) ==
              FrameStatus::kIncomplete);
        CHECK(consumed == 0);
    }
}

TEST_CASE("several frames decode from one buffer", "[ws]") {
    // TCP delivers a byte stream, not messages: a single read routinely
    // contains several frames, or one and a half.
    std::string bytes = server_frame(Opcode::kText, "one");
    bytes += server_frame(Opcode::kText, "two");
    bytes += server_frame(Opcode::kPing, "");

    std::string_view remaining = bytes;
    std::vector<std::string> payloads;
    for (;;) {
        Frame frame{};
        std::size_t consumed = 0;
        const FrameStatus status = decode_frame(remaining, frame, consumed);
        if (status != FrameStatus::kOk) {
            break;
        }
        payloads.emplace_back(frame.payload);
        remaining.remove_prefix(consumed);
    }
    REQUIRE(payloads.size() == 3);
    CHECK(payloads[0] == "one");
    CHECK(payloads[1] == "two");
    CHECK(remaining.empty());
}

// ---------------------------------------------------------------------------
// Protocol violations
// ---------------------------------------------------------------------------

TEST_CASE("a masked server frame is rejected", "[ws][protocol]") {
    // RFC 6455 5.1: a server MUST NOT mask. Rejecting rather than unmasking
    // keeps the payload a zero-copy view, and a peer that violates framing
    // cannot be trusted about anything else.
    std::string bytes = server_frame(Opcode::kText, "hello");
    bytes[1] = static_cast<char>(static_cast<std::uint8_t>(bytes[1]) | 0x80u);

    Frame frame{};
    std::size_t consumed = 0;
    CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
}

TEST_CASE("reserved bits are rejected", "[ws][protocol]") {
    constexpr std::uint8_t kRsvBits[] = {0x10, 0x20, 0x40};
    for (std::uint8_t rsv : kRsvBits) {
        std::string bytes = server_frame(Opcode::kText, "hi");
        bytes[0] = static_cast<char>(static_cast<std::uint8_t>(bytes[0]) | rsv);
        Frame frame{};
        std::size_t consumed = 0;
        INFO("rsv bit 0x" << std::hex << static_cast<int>(rsv));
        CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
    }
}

TEST_CASE("reserved opcodes are rejected", "[ws][protocol]") {
    constexpr std::uint8_t kReserved[] = {0x3, 0x4, 0x7, 0xB, 0xF};
    for (std::uint8_t op : kReserved) {
        std::string bytes;
        bytes.push_back(static_cast<char>(0x80u | op));
        bytes.push_back(static_cast<char>(0));
        Frame frame{};
        std::size_t consumed = 0;
        INFO("opcode 0x" << std::hex << static_cast<int>(op));
        CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
    }
}

TEST_CASE("fragmented control frames are rejected", "[ws][protocol]") {
    // Control frames must be self-contained: a peer cannot be allowed to
    // suspend a ping across a message boundary.
    std::string bytes = server_frame(Opcode::kPing, "x", /*fin=*/false);
    Frame frame{};
    std::size_t consumed = 0;
    CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
}

TEST_CASE("oversized control payloads are rejected", "[ws][protocol]") {
    const std::string payload(126, 'x');  // Limit is 125.
    const std::string bytes = server_frame(Opcode::kClose, payload);
    Frame frame{};
    std::size_t consumed = 0;
    CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
}

TEST_CASE("non-minimal length encodings are rejected", "[ws][protocol]") {
    // A peer that can express one length two ways can smuggle a different
    // parse past anything checking only the first form.
    SECTION("16-bit form used for a length under 126") {
        std::string bytes;
        bytes.push_back(static_cast<char>(0x81));  // FIN + text
        bytes.push_back(static_cast<char>(126));
        bytes.push_back(0);
        bytes.push_back(5);  // Should have used the 7-bit form.
        bytes += "hello";
        Frame frame{};
        std::size_t consumed = 0;
        CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
    }
    SECTION("64-bit form used for a length under 65536") {
        std::string bytes;
        bytes.push_back(static_cast<char>(0x81));
        bytes.push_back(static_cast<char>(127));
        for (int i = 0; i < 6; ++i) {
            bytes.push_back(0);
        }
        bytes.push_back(0);
        bytes.push_back(5);
        bytes += "hello";
        Frame frame{};
        std::size_t consumed = 0;
        CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
    }
}

TEST_CASE("a 64-bit length with the top bit set is rejected", "[ws][protocol]") {
    // RFC 6455 5.2 forbids it. Accepting would mean sign-confusing the length
    // into something that indexes wherever it likes.
    std::string bytes;
    bytes.push_back(static_cast<char>(0x82));
    bytes.push_back(static_cast<char>(127));
    bytes.push_back(static_cast<char>(0x80));
    for (int i = 0; i < 7; ++i) {
        bytes.push_back(0);
    }
    Frame frame{};
    std::size_t consumed = 0;
    CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kProtocolError);
}

TEST_CASE("an implausibly large frame is refused, not allocated", "[ws][protocol]") {
    std::string bytes;
    bytes.push_back(static_cast<char>(0x82));
    bytes.push_back(static_cast<char>(127));
    const std::uint64_t huge = 1ull << 40;  // 1 TiB
    for (int i = 7; i >= 0; --i) {
        bytes.push_back(static_cast<char>((huge >> (i * 8)) & 0xFF));
    }
    Frame frame{};
    std::size_t consumed = 0;
    CHECK(decode_frame(bytes, frame, consumed) == FrameStatus::kTooLarge);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

TEST_CASE("client frames are masked", "[ws]") {
    // RFC 6455 requires it, and an unmasked client frame is closed on by every
    // conforming server.
    std::vector<char> out;
    encode_frame(Opcode::kText, "hello", 0x37FA213D, out);

    REQUIRE(out.size() == 2 + 4 + 5);
    CHECK((static_cast<std::uint8_t>(out[0]) & 0x0F) == 0x1);
    CHECK((static_cast<std::uint8_t>(out[0]) & 0x80) != 0);   // FIN
    CHECK((static_cast<std::uint8_t>(out[1]) & 0x80) != 0);   // MASK
    CHECK((static_cast<std::uint8_t>(out[1]) & 0x7F) == 5);

    // Unmask and confirm the round trip.
    const std::uint8_t key[4] = {0x37, 0xFA, 0x21, 0x3D};
    std::string decoded;
    for (std::size_t i = 0; i < 5; ++i) {
        decoded.push_back(static_cast<char>(static_cast<std::uint8_t>(out[6 + i]) ^ key[i % 4]));
    }
    CHECK(decoded == "hello");
}

TEST_CASE("client frames use minimal length encodings", "[ws]") {
    std::vector<char> small;
    encode_frame(Opcode::kText, std::string(10, 'x'), 0, small);
    CHECK((static_cast<std::uint8_t>(small[1]) & 0x7F) == 10);

    std::vector<char> medium;
    encode_frame(Opcode::kText, std::string(1000, 'x'), 0, medium);
    CHECK((static_cast<std::uint8_t>(medium[1]) & 0x7F) == 126);

    std::vector<char> large;
    encode_frame(Opcode::kText, std::string(70'000, 'x'), 0, large);
    CHECK((static_cast<std::uint8_t>(large[1]) & 0x7F) == 127);
}

// ---------------------------------------------------------------------------
// Fragmentation
// ---------------------------------------------------------------------------

TEST_CASE("an unfragmented message passes through without a copy", "[ws][assembly]") {
    MessageAssembler assembler;
    const std::string bytes = server_frame(Opcode::kText, "complete");
    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);

    MessageAssembler::Message message{};
    REQUIRE(assembler.accept(frame, message));
    CHECK(message.payload == "complete");
    CHECK(message.payload.data() == frame.payload.data());  // Same bytes, no copy.
}

TEST_CASE("fragments reassemble in order", "[ws][assembly]") {
    // A venue pushing a large book snapshot will fragment it, and a client that
    // assumes one frame per message truncates exactly the message it most needs.
    MessageAssembler assembler;
    MessageAssembler::Message message{};

    const std::string first = server_frame(Opcode::kText, "Hello, ", /*fin=*/false);
    const std::string middle = server_frame(Opcode::kContinuation, "brave ", /*fin=*/false);
    const std::string last = server_frame(Opcode::kContinuation, "world");

    for (const std::string* part : {&first, &middle}) {
        Frame frame{};
        std::size_t consumed = 0;
        REQUIRE(decode_frame(*part, frame, consumed) == FrameStatus::kOk);
        CHECK_FALSE(assembler.accept(frame, message));
    }

    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(last, frame, consumed) == FrameStatus::kOk);
    REQUIRE(assembler.accept(frame, message));
    CHECK(message.payload == "Hello, brave world");
    CHECK(message.opcode == Opcode::kText);  // Taken from the first fragment.
}

TEST_CASE("control frames interleave inside a fragmented message", "[ws][assembly]") {
    // Explicitly permitted, and a client that appends the ping into the message
    // corrupts it in a way that looks like venue corruption.
    MessageAssembler assembler;
    MessageAssembler::Message message{};

    const std::string start = server_frame(Opcode::kText, "part1", /*fin=*/false);
    const std::string ping = server_frame(Opcode::kPing, "hb");
    const std::string end = server_frame(Opcode::kContinuation, "part2");

    Frame frame{};
    std::size_t consumed = 0;

    REQUIRE(decode_frame(start, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));

    REQUIRE(decode_frame(ping, frame, consumed) == FrameStatus::kOk);
    REQUIRE(assembler.accept(frame, message));
    CHECK(message.is_control);
    CHECK(message.opcode == Opcode::kPing);

    REQUIRE(decode_frame(end, frame, consumed) == FrameStatus::kOk);
    REQUIRE(assembler.accept(frame, message));
    CHECK_FALSE(message.is_control);
    CHECK(message.payload == "part1part2");  // The ping did not contaminate it.
}

TEST_CASE("a stray continuation is a protocol error", "[ws][assembly]") {
    MessageAssembler assembler;
    MessageAssembler::Message message{};
    const std::string bytes = server_frame(Opcode::kContinuation, "orphan");
    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(bytes, frame, consumed) == FrameStatus::kOk);

    CHECK_FALSE(assembler.accept(frame, message));
    CHECK(assembler.failed());
}

TEST_CASE("a new message before the last finished is a protocol error", "[ws][assembly]") {
    MessageAssembler assembler;
    MessageAssembler::Message message{};

    const std::string start = server_frame(Opcode::kText, "part", /*fin=*/false);
    const std::string interrupt = server_frame(Opcode::kText, "new");

    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(start, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));

    REQUIRE(decode_frame(interrupt, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));
    CHECK(assembler.failed());
}

TEST_CASE("reassembly is bounded", "[ws][assembly]") {
    // A peer can otherwise stream fragments forever and exhaust memory without
    // ever sending an oversized frame.
    MessageAssembler assembler(64);
    MessageAssembler::Message message{};

    const std::string start = server_frame(Opcode::kText, std::string(40, 'a'), /*fin=*/false);
    const std::string more = server_frame(Opcode::kContinuation, std::string(40, 'b'));

    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(start, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));

    REQUIRE(decode_frame(more, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));
    CHECK(assembler.failed());
}

TEST_CASE("reset clears a failed assembler", "[ws][assembly]") {
    MessageAssembler assembler;
    MessageAssembler::Message message{};
    const std::string orphan = server_frame(Opcode::kContinuation, "x");
    Frame frame{};
    std::size_t consumed = 0;
    REQUIRE(decode_frame(orphan, frame, consumed) == FrameStatus::kOk);
    CHECK_FALSE(assembler.accept(frame, message));
    REQUIRE(assembler.failed());

    assembler.reset();
    CHECK_FALSE(assembler.failed());

    const std::string good = server_frame(Opcode::kText, "fine");
    REQUIRE(decode_frame(good, frame, consumed) == FrameStatus::kOk);
    CHECK(assembler.accept(frame, message));
}

TEST_CASE("close codes are read, and an empty close means 1005", "[ws]") {
    std::string payload;
    payload.push_back(static_cast<char>(0x03));
    payload.push_back(static_cast<char>(0xE8));  // 1000, normal closure
    CHECK(close_code(payload) == 1000);

    CHECK(close_code("") == 1005);   // No status present.
    CHECK(close_code("x") == 1005);  // Malformed; not a code.
}
