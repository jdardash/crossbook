// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// RFC 6455 framing.
//
// The bar here is higher than "it decodes a message", because this is the layer
// that reads a length off a socket and then trusts it. Every test that asserts a
// rejection is asserting a bound that is not being exceeded.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

#include "crossbook/net/ws_frame.hpp"

using namespace crossbook::net;

namespace {

/// Build a server-to-client frame: FIN set, unmasked, minimal length encoding.
std::string server_frame(Opcode opcode, std::string_view payload, bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80U : 0x00U) | static_cast<std::uint8_t>(opcode)));

    if (payload.size() < 126) {
        out.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((payload.size() >> 8) & 0xFFU));
        out.push_back(static_cast<char>(payload.size() & 0xFFU));
    } else {
        out.push_back(static_cast<char>(127));
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((payload.size() >> shift) & 0xFFU));
        }
    }
    out.append(payload);
    return out;
}

}  // namespace

TEST_CASE("A short unmasked frame header parses") {
    const std::string frame = server_frame(Opcode::kText, "hello");
    FrameHeader header;
    REQUIRE(parse_frame_header(frame, header) == FrameStatus::kOk);
    CHECK(header.fin);
    CHECK_FALSE(header.masked);
    CHECK(header.opcode == Opcode::kText);
    CHECK(header.payload_len == 5);
    CHECK(header.header_size == 2);
}

TEST_CASE("Every truncation of a header reports incomplete, never a parse") {
    // 70 KiB forces the 64-bit length path, so the check covers all three
    // header shapes as the buffer grows one byte at a time.
    const std::string payload(70000, 'x');
    const std::string frame = server_frame(Opcode::kBinary, payload);

    for (std::size_t prefix = 0; prefix < 10; ++prefix) {
        FrameHeader header;
        const FrameStatus status = parse_frame_header(std::string_view(frame).substr(0, prefix), header);
        INFO("prefix length " << prefix);
        CHECK(status == FrameStatus::kIncomplete);
    }

    FrameHeader header;
    REQUIRE(parse_frame_header(frame, header) == FrameStatus::kOk);
    CHECK(header.payload_len == 70000);
    CHECK(header.header_size == 10);
}

TEST_CASE("Reserved bits are rejected") {
    std::string frame = server_frame(Opcode::kText, "hi");
    frame[0] = static_cast<char>(static_cast<unsigned char>(frame[0]) | 0x40U);  // RSV1
    FrameHeader header;
    CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
}

TEST_CASE("Unknown opcodes are rejected rather than ignored") {
    std::string frame = server_frame(Opcode::kText, "hi");
    frame[0] = static_cast<char>(0x80U | 0x03U);  // Reserved data opcode.
    FrameHeader header;
    CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
}

TEST_CASE("Non-minimal length encodings are rejected") {
    SECTION("16-bit form used for a length that fits in 7 bits") {
        // A payload of 5 spelled with the 126 escape.
        const std::string frame = std::string("\x81\x7e\x00\x05hello", 9);
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
    }
    SECTION("64-bit form used for a length that fits in 16 bits") {
        std::string frame("\x81\x7f", 2);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((200ULL >> shift) & 0xFFU));
        }
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
    }
    SECTION("64-bit length with the top bit set is rejected") {
        std::string frame("\x81\x7f", 2);
        frame.push_back(static_cast<char>(0x80));
        for (int i = 0; i < 7; ++i) {
            frame.push_back('\0');
        }
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
    }
}

TEST_CASE("Control frames must be short and unfragmented") {
    SECTION("a fragmented ping is a protocol error") {
        const std::string frame = server_frame(Opcode::kPing, "x", /*fin=*/false);
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
    }
    SECTION("a ping over 125 bytes is a protocol error") {
        const std::string frame = server_frame(Opcode::kPing, std::string(126, 'x'));
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kProtocolError);
    }
    SECTION("exactly 125 bytes is allowed") {
        const std::string frame = server_frame(Opcode::kPing, std::string(125, 'x'));
        FrameHeader header;
        CHECK(parse_frame_header(frame, header) == FrameStatus::kOk);
    }
}

TEST_CASE("Masking is its own inverse, and offsets keep a split payload aligned") {
    const std::string original = "the quick brown fox jumps over the lazy dog";
    const std::uint32_t key = 0xDEADBEEF;

    std::string buffer = original;
    apply_mask(buffer.data(), buffer.size(), key);
    CHECK(buffer != original);
    apply_mask(buffer.data(), buffer.size(), key);
    CHECK(buffer == original);

    // Unmasking in two pieces must match unmasking in one, or a payload split
    // across two reads decodes to garbage from the split point onward.
    std::string whole = original;
    apply_mask(whole.data(), whole.size(), key);

    std::string split = whole;
    constexpr std::size_t kCut = 7;  // Deliberately not a multiple of 4.
    apply_mask(split.data(), kCut, key, 0);
    apply_mask(split.data() + kCut, split.size() - kCut, key, kCut);
    CHECK(split == original);
}

TEST_CASE("A written header round-trips through the parser") {
    for (const std::uint64_t length : {std::uint64_t{0}, std::uint64_t{125}, std::uint64_t{126},
                                       std::uint64_t{65535}, std::uint64_t{65536},
                                       std::uint64_t{1'000'000}}) {
        char buffer[kMaxHeaderSize];
        const std::size_t written =
            write_frame_header(buffer, Opcode::kText, length, 0x11223344U);

        FrameHeader header;
        REQUIRE(parse_frame_header(std::string_view(buffer, written), header) == FrameStatus::kOk);
        INFO("payload length " << length);
        CHECK(header.payload_len == length);
        CHECK(header.masked);  // A client MUST mask; the writer always does.
        CHECK(header.mask_key == 0x11223344U);
        CHECK(header.header_size == written);
    }
}

TEST_CASE("The reader returns a whole message") {
    FrameReader reader;
    const std::string frame = server_frame(Opcode::kText, R"({"channel":"book"})");
    reader.append(frame);

    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.opcode == Opcode::kText);
    CHECK(event.payload == R"({"channel":"book"})");
    CHECK(reader.next(event) == ReadStatus::kNeedMore);
}

TEST_CASE("A message split across transport reads is reassembled") {
    // Byte at a time is the pathological case, and it is the one that shakes out
    // off-by-ones in the buffer bookkeeping. A real socket will split a frame at
    // an arbitrary point, so "arbitrary" is tested as "every point".
    const std::string payload(500, 'a');
    const std::string frame = server_frame(Opcode::kText, payload);

    FrameReader reader;
    Event event;
    for (std::size_t i = 0; i + 1 < frame.size(); ++i) {
        reader.append(frame.data() + i, 1);
        REQUIRE(reader.next(event) == ReadStatus::kNeedMore);
    }
    reader.append(frame.data() + frame.size() - 1, 1);
    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.payload == payload);
}

TEST_CASE("Fragmented messages are joined") {
    FrameReader reader;
    reader.append(server_frame(Opcode::kText, "part one ", /*fin=*/false));
    reader.append(server_frame(Opcode::kContinuation, "part two ", /*fin=*/false));
    reader.append(server_frame(Opcode::kContinuation, "part three"));

    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.opcode == Opcode::kText);
    CHECK(event.payload == "part one part two part three");
}

TEST_CASE("A ping between fragments does not corrupt the message") {
    // This is the case a reader that treats every frame as a message gets wrong,
    // and it is not hypothetical: venues ping on a timer regardless of what they
    // are in the middle of sending.
    FrameReader reader;
    reader.append(server_frame(Opcode::kText, "before ", /*fin=*/false));
    reader.append(server_frame(Opcode::kPing, "keepalive"));
    reader.append(server_frame(Opcode::kContinuation, "after"));

    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kPing);
    CHECK(event.payload == "keepalive");
    CHECK(reader.assembling());

    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.payload == "before after");
}

TEST_CASE("Close frames carry a code when they have one") {
    SECTION("with a code and reason") {
        FrameReader reader;
        reader.append(server_frame(Opcode::kClose, std::string("\x03\xe8", 2) + "bye"));
        Event event;
        REQUIRE(reader.next(event) == ReadStatus::kClose);
        CHECK(event.close_code == 1000);
        CHECK(event.payload == "bye");
    }
    SECTION("empty close reports 1005, which is not the same as 1000") {
        FrameReader reader;
        reader.append(server_frame(Opcode::kClose, ""));
        Event event;
        REQUIRE(reader.next(event) == ReadStatus::kClose);
        CHECK(event.close_code == static_cast<std::uint16_t>(CloseCode::kNoStatus));
    }
    SECTION("a one-byte close payload is malformed") {
        FrameReader reader;
        reader.append(server_frame(Opcode::kClose, "\x03"));
        Event event;
        CHECK(reader.next(event) == ReadStatus::kProtocolError);
    }
}

TEST_CASE("Protocol violations from the server are rejected") {
    SECTION("a continuation with nothing to continue") {
        FrameReader reader;
        reader.append(server_frame(Opcode::kContinuation, "orphan"));
        Event event;
        CHECK(reader.next(event) == ReadStatus::kProtocolError);
    }
    SECTION("a new data frame while a message is still assembling") {
        FrameReader reader;
        reader.append(server_frame(Opcode::kText, "first", /*fin=*/false));
        reader.append(server_frame(Opcode::kText, "second"));
        Event event;
        CHECK(reader.next(event) == ReadStatus::kProtocolError);
    }
    SECTION("a masked frame from the server") {
        // Servers must not mask. A masked frame means we have lost alignment.
        std::string frame = server_frame(Opcode::kText, "hi");
        frame[1] = static_cast<char>(static_cast<unsigned char>(frame[1]) | 0x80U);
        frame.insert(2, 4, '\0');  // Make room for the mask key.
        FrameReader reader;
        reader.append(frame);
        Event event;
        CHECK(reader.next(event) == ReadStatus::kProtocolError);
    }
}

TEST_CASE("An oversized message is refused before its bytes are buffered") {
    // The declared length is what is rejected, not the accumulated bytes: a
    // ceiling that only trips after the payload has been read is not a ceiling.
    FrameReader reader(1024);

    std::string header("\x82\x7f", 2);
    for (int shift = 56; shift >= 0; shift -= 8) {
        header.push_back(static_cast<char>((4'000'000'000ULL >> shift) & 0xFFU));
    }
    reader.append(header);

    Event event;
    CHECK(reader.next(event) == ReadStatus::kMessageTooLarge);
    CHECK(reader.buffered() == header.size());  // Nothing was consumed.
}

TEST_CASE("A fragmented message is bounded by the same ceiling as a single frame") {
    FrameReader reader(300);
    reader.append(server_frame(Opcode::kBinary, std::string(200, 'x'), /*fin=*/false));

    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kNeedMore);

    reader.append(server_frame(Opcode::kContinuation, std::string(200, 'y')));
    CHECK(reader.next(event) == ReadStatus::kMessageTooLarge);
}

TEST_CASE("Many messages in one buffer are drained in order") {
    FrameReader reader;
    std::string stream;
    for (int i = 0; i < 100; ++i) {
        stream += server_frame(Opcode::kText, "msg" + std::to_string(i));
    }
    reader.append(stream);

    Event event;
    for (int i = 0; i < 100; ++i) {
        REQUIRE(reader.next(event) == ReadStatus::kMessage);
        CHECK(event.payload == "msg" + std::to_string(i));
    }
    CHECK(reader.next(event) == ReadStatus::kNeedMore);
    CHECK(reader.buffered() == 0);
}

TEST_CASE("A protocol error is terminal, not a hiccup to recover from") {
    // After a bad length, the next byte read as an opcode is whatever happened
    // to be there. Recovering would mean inventing frames, and a book built
    // from invented frames looks entirely plausible.
    FrameReader reader;
    reader.append(server_frame(Opcode::kContinuation, "orphan"));

    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kProtocolError);
    CHECK(reader.failed());

    reader.append(server_frame(Opcode::kText, "perfectly valid"));
    CHECK(reader.next(event) == ReadStatus::kProtocolError);
    CHECK(reader.next(event) == ReadStatus::kProtocolError);

    reader.reset();
    CHECK_FALSE(reader.failed());
    reader.append(server_frame(Opcode::kText, "fresh start"));
    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.payload == "fresh start");
}

TEST_CASE("An empty payload is a message, not a non-event") {
    FrameReader reader;
    reader.append(server_frame(Opcode::kText, ""));
    Event event;
    REQUIRE(reader.next(event) == ReadStatus::kMessage);
    CHECK(event.payload.empty());
}
