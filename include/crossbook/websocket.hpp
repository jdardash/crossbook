// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// RFC 6455 WebSocket frame codec.
//
// WHERE THE TRANSPORT BOUNDARY SITS, AND WHY
//
// crossbook does not open sockets. TLS would end the zero-dependency property
// that makes a header-only library adoptable, and socket plumbing is both the
// least interesting part and the part every adopter already has.
//
// But "bring your own transport" is not an excuse to skip the part that is
// actually easy to get wrong. Framing is where the bugs live: length fields
// that lie, masking that a server must never apply, fragmentation, control
// frames interleaved mid-message, and a 64-bit length whose top bit the spec
// forbids. All of that is pure byte manipulation with no I/O, so it belongs
// here — testable offline, fuzzable, and dependency-free.
//
// What you supply is a byte stream. What this gives you is messages.
//
// FRAME LAYOUT
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-------+-+-------------+-------------------------------+
//    |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
//    |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
//    |N|V|V|V|       |S|             |                               |
//    | |1|2|3|       |K|             |                               |
//    +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
//    |     Masking-key (0 or 4 bytes)  |          Payload Data       |
//    +---------------------------------------------------------------+

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace crossbook::ws {

/// RFC 6455 section 5.2 opcodes.
enum class Opcode : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

[[nodiscard]] constexpr bool is_control(Opcode op) noexcept {
    return (static_cast<std::uint8_t>(op) & 0x8u) != 0;
}

[[nodiscard]] constexpr std::string_view to_string(Opcode op) noexcept {
    switch (op) {
        case Opcode::kContinuation:
            return "continuation";
        case Opcode::kText:
            return "text";
        case Opcode::kBinary:
            return "binary";
        case Opcode::kClose:
            return "close";
        case Opcode::kPing:
            return "ping";
        case Opcode::kPong:
            return "pong";
    }
    return "unknown";
}

enum class FrameStatus : std::uint8_t {
    /// A complete frame was decoded.
    kOk,
    /// Not enough bytes yet. Read more and call again with a longer buffer.
    /// Never an error — it is the normal state of a stream.
    kIncomplete,
    /// The peer violated the protocol. The connection must be closed: a peer
    /// that cannot frame correctly cannot be trusted about anything else.
    kProtocolError,
    /// The frame declares a payload larger than this decoder accepts.
    kTooLarge,
};

[[nodiscard]] constexpr std::string_view to_string(FrameStatus s) noexcept {
    switch (s) {
        case FrameStatus::kOk:
            return "ok";
        case FrameStatus::kIncomplete:
            return "incomplete";
        case FrameStatus::kProtocolError:
            return "protocol_error";
        case FrameStatus::kTooLarge:
            return "too_large";
    }
    return "unknown";
}

/// One decoded frame. `payload` views into the caller's buffer.
struct Frame {
    Opcode opcode{Opcode::kContinuation};
    bool fin{false};
    std::string_view payload;
};

/// Largest single frame accepted by default. Venue frames are kilobytes; a
/// peer announcing gigabytes is either broken or hostile, and either way the
/// answer is not to allocate it.
inline constexpr std::uint64_t kDefaultMaxFrame = 16ull * 1024ull * 1024ull;

/// Decode one frame from the front of `buffer`.
///
/// On kOk, `consumed` is the number of bytes the frame occupied; the caller
/// removes them and calls again. On kIncomplete, nothing is consumed.
///
/// Server-to-client frames MUST NOT be masked (RFC 6455 section 5.1). A masked
/// frame from a server is rejected rather than unmasked, which keeps `payload`
/// a view into the caller's buffer with no copy — and refusing a protocol
/// violation is better behaviour than quietly accommodating it.
[[nodiscard]] inline FrameStatus decode_frame(std::string_view buffer, Frame& out,
                                              std::size_t& consumed,
                                              std::uint64_t max_frame = kDefaultMaxFrame) {
    consumed = 0;
    if (buffer.size() < 2) {
        return FrameStatus::kIncomplete;
    }

    const auto byte0 = static_cast<std::uint8_t>(buffer[0]);
    const auto byte1 = static_cast<std::uint8_t>(buffer[1]);

    // RSV1-3 must be zero absent a negotiated extension, and we negotiate none.
    if ((byte0 & 0x70u) != 0) {
        return FrameStatus::kProtocolError;
    }

    const auto opcode_bits = static_cast<std::uint8_t>(byte0 & 0x0Fu);
    switch (opcode_bits) {
        case 0x0:
        case 0x1:
        case 0x2:
        case 0x8:
        case 0x9:
        case 0xA:
            break;
        default:
            return FrameStatus::kProtocolError;  // Reserved opcode.
    }
    const auto opcode = static_cast<Opcode>(opcode_bits);
    const bool fin = (byte0 & 0x80u) != 0;
    const bool masked = (byte1 & 0x80u) != 0;

    // Control frames carry at most 125 bytes and must never be fragmented.
    if (is_control(opcode)) {
        if (!fin || (byte1 & 0x7Fu) > 125) {
            return FrameStatus::kProtocolError;
        }
    }

    std::uint64_t length = byte1 & 0x7Fu;
    std::size_t offset = 2;

    if (length == 126) {
        if (buffer.size() < offset + 2) {
            return FrameStatus::kIncomplete;
        }
        length = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[offset])) << 8) |
                 static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[offset + 1]));
        offset += 2;
        // The spec requires the minimal length encoding.
        if (length < 126) {
            return FrameStatus::kProtocolError;
        }
    } else if (length == 127) {
        if (buffer.size() < offset + 8) {
            return FrameStatus::kIncomplete;
        }
        length = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            length = (length << 8) |
                     static_cast<std::uint64_t>(static_cast<std::uint8_t>(buffer[offset + i]));
        }
        offset += 8;
        // The most significant bit MUST be 0 (RFC 6455 section 5.2), and the
        // minimal encoding is again required.
        if ((length >> 63) != 0 || length <= 0xFFFF) {
            return FrameStatus::kProtocolError;
        }
    }

    if (masked) {
        // A server must not mask. Rejecting keeps the payload zero-copy.
        return FrameStatus::kProtocolError;
    }
    if (length > max_frame) {
        return FrameStatus::kTooLarge;
    }
    if (buffer.size() < offset + length) {
        return FrameStatus::kIncomplete;
    }

    out.opcode = opcode;
    out.fin = fin;
    out.payload = buffer.substr(offset, static_cast<std::size_t>(length));
    consumed = offset + static_cast<std::size_t>(length);
    return FrameStatus::kOk;
}

/// Encode a client-to-server frame into `out`, appending.
///
/// Client frames MUST be masked with a key the peer cannot predict. `key` is
/// taken as a parameter rather than generated here because this header has no
/// business owning a random source, and a hardcoded or predictable key is a
/// real vulnerability rather than a style question.
inline void encode_frame(Opcode opcode, std::string_view payload, std::uint32_t mask_key,
                         std::vector<char>& out, bool fin = true) {
    out.push_back(static_cast<char>((fin ? 0x80u : 0x00u) |
                                    (static_cast<std::uint8_t>(opcode) & 0x0Fu)));

    const std::uint64_t length = payload.size();
    if (length < 126) {
        out.push_back(static_cast<char>(0x80u | length));  // Mask bit always set.
    } else if (length <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80u | 126u));
        out.push_back(static_cast<char>((length >> 8) & 0xFFu));
        out.push_back(static_cast<char>(length & 0xFFu));
    } else {
        out.push_back(static_cast<char>(0x80u | 127u));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((length >> (i * 8)) & 0xFFu));
        }
    }

    // Written out rather than computed in a loop: index arithmetic mixing int
    // and size_t is what broke the GCC build on the length decoder, and there
    // is nothing to gain from repeating the pattern for four constants.
    const std::uint8_t key[4] = {
        static_cast<std::uint8_t>((mask_key >> 24) & 0xFFu),
        static_cast<std::uint8_t>((mask_key >> 16) & 0xFFu),
        static_cast<std::uint8_t>((mask_key >> 8) & 0xFFu),
        static_cast<std::uint8_t>(mask_key & 0xFFu),
    };
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>(key[i]));
    }
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4]));
    }
}

/// Reassembles fragmented messages and surfaces control frames separately.
///
/// Fragmentation is not exotic: a venue pushing a large book snapshot will
/// split it, and a client that assumes one frame per message silently truncates
/// exactly the message it most needs to get right. Control frames may also be
/// interleaved *inside* a fragmented message, so they cannot simply be appended.
class MessageAssembler {
public:
    struct Message {
        Opcode opcode{Opcode::kText};
        std::string_view payload;
        bool is_control{false};
    };

    explicit MessageAssembler(std::uint64_t max_message = kDefaultMaxFrame)
        : max_message_(max_message) {}

    /// Feed one decoded frame. Returns true when `out` holds a complete
    /// message; false when more fragments are needed.
    ///
    /// `out.payload` may view either the frame (unfragmented, the common case,
    /// and copy-free) or the internal buffer (reassembled).
    [[nodiscard]] bool accept(const Frame& frame, Message& out) {
        if (is_control(frame.opcode)) {
            // Control frames are never fragmented and never disturb a message
            // in progress.
            out.opcode = frame.opcode;
            out.payload = frame.payload;
            out.is_control = true;
            return true;
        }

        if (!in_progress_) {
            if (frame.opcode == Opcode::kContinuation) {
                error_ = true;  // Continuation with nothing to continue.
                return false;
            }
            if (frame.fin) {
                // The overwhelming majority of frames. No copy at all.
                out.opcode = frame.opcode;
                out.payload = frame.payload;
                out.is_control = false;
                return true;
            }
            in_progress_ = true;
            message_opcode_ = frame.opcode;
            buffer_.assign(frame.payload.begin(), frame.payload.end());
            return false;
        }

        if (frame.opcode != Opcode::kContinuation) {
            error_ = true;  // A new message started before the last finished.
            return false;
        }
        if (buffer_.size() + frame.payload.size() > max_message_) {
            error_ = true;
            return false;
        }
        buffer_.insert(buffer_.end(), frame.payload.begin(), frame.payload.end());

        if (!frame.fin) {
            return false;
        }
        in_progress_ = false;
        out.opcode = message_opcode_;
        out.payload = std::string_view(buffer_.data(), buffer_.size());
        out.is_control = false;
        return true;
    }

    /// True once a protocol violation has been seen. The connection should be
    /// closed; continuing would mean trusting a peer that cannot frame.
    [[nodiscard]] bool failed() const noexcept { return error_; }

    void reset() noexcept {
        in_progress_ = false;
        error_ = false;
        buffer_.clear();
    }

private:
    std::uint64_t max_message_;
    std::vector<char> buffer_;
    Opcode message_opcode_{Opcode::kText};
    bool in_progress_{false};
    bool error_{false};
};

/// The close code from a close frame's payload, if it carries one.
/// A close frame may legitimately be empty, which means 1005 "no status".
[[nodiscard]] inline std::uint16_t close_code(std::string_view payload) noexcept {
    if (payload.size() < 2) {
        return 1005;
    }
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0])) << 8) |
        static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1])));
}

}  // namespace crossbook::ws
