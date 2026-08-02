// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// RFC 6455 frame codec — the pure part of a websocket client.
//
// WHY THIS IS ITS OWN FILE, WITH NO I/O IN IT:
//
// Everything here is a function of bytes. No socket, no TLS, no clock, no
// platform header. That is what lets the framing layer — the part actually
// exposed to bytes an exchange controls — be unit tested exhaustively and
// fuzzed, while the parts that cannot be fuzzed (the handshake, the socket) stay
// as thin as possible around it.
//
// The threat model is not academic. A feed handler parses length-prefixed
// binary from a remote host on every message, and a 64-bit length field read
// without bounds discipline is the oldest remote-code-execution shape there is.
// So: every length is validated before it is trusted, the reassembly buffer has
// a hard ceiling, and `fuzz/fuzz_ws_frame.cpp` drives this state machine with
// coverage-guided garbage.
//
// STRICTNESS IS DELIBERATE. The RFC's "MUST" list is enforced rather than
// tolerated — reserved bits, non-minimal length encodings, fragmented control
// frames, masked server frames. A frame the specification forbids is either a
// broken venue or something wearing a venue's clothes, and quietly accepting it
// means the book is being built from bytes nobody has a contract for.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace crossbook::net {

/// RFC 6455 §5.2 opcodes.
enum class Opcode : std::uint8_t {
    kContinuation = 0x0,
    kText = 0x1,
    kBinary = 0x2,
    kClose = 0x8,
    kPing = 0x9,
    kPong = 0xA,
};

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

/// Control frames have the high opcode bit set (§5.5). They may be interleaved
/// between the fragments of a data message, which is the detail a naive reader
/// gets wrong: a ping arriving mid-message must not terminate the message.
[[nodiscard]] constexpr bool is_control(Opcode op) noexcept {
    return (static_cast<std::uint8_t>(op) & 0x08U) != 0;
}

[[nodiscard]] constexpr bool is_known_opcode(std::uint8_t raw) noexcept {
    return raw == 0x0 || raw == 0x1 || raw == 0x2 || raw == 0x8 || raw == 0x9 || raw == 0xA;
}

/// Control frame payloads are capped by the specification, not by us (§5.5).
inline constexpr std::size_t kMaxControlPayload = 125;

/// Largest frame header: 2 fixed bytes + 8 length bytes + 4 mask bytes.
inline constexpr std::size_t kMaxHeaderSize = 14;

struct FrameHeader {
    Opcode opcode{Opcode::kContinuation};
    std::uint64_t payload_len{0};
    /// Big-endian as it appeared on the wire; `apply_mask` expects this form.
    std::uint32_t mask_key{0};
    /// Bytes consumed by the header itself.
    std::size_t header_size{0};
    bool fin{false};
    bool masked{false};
};

enum class FrameStatus : std::uint8_t {
    kOk,
    /// Not enough bytes yet. Read more and call again with a longer buffer.
    kIncomplete,
    /// The bytes are not a legal frame. The connection must be closed; there is
    /// no resynchronisation point in a stream of length-prefixed frames.
    kProtocolError,
};

/// Parse a frame header from the front of `buf`.
///
/// Never reads past `buf.size()`, and never trusts a length field before it has
/// been range-checked. `kIncomplete` is returned for any truncation, including a
/// buffer holding only part of the extended length.
[[nodiscard]] inline FrameStatus parse_frame_header(std::string_view buf,
                                                    FrameHeader& out) noexcept {
    if (buf.size() < 2) {
        return FrameStatus::kIncomplete;
    }

    const auto b0 = static_cast<std::uint8_t>(buf[0]);
    const auto b1 = static_cast<std::uint8_t>(buf[1]);

    // RSV1-3 must be zero: they only carry meaning under an extension, and we
    // negotiate none. Set bits mean we are misreading the stream.
    if ((b0 & 0x70U) != 0) {
        return FrameStatus::kProtocolError;
    }

    const auto raw_opcode = static_cast<std::uint8_t>(b0 & 0x0FU);
    if (!is_known_opcode(raw_opcode)) {
        return FrameStatus::kProtocolError;
    }

    FrameHeader header;
    header.fin = (b0 & 0x80U) != 0;
    header.opcode = static_cast<Opcode>(raw_opcode);
    header.masked = (b1 & 0x80U) != 0;

    const auto short_len = static_cast<std::uint8_t>(b1 & 0x7FU);
    std::size_t pos = 2;

    if (short_len < 126) {
        header.payload_len = short_len;
    } else if (short_len == 126) {
        if (buf.size() < pos + 2) {
            return FrameStatus::kIncomplete;
        }
        header.payload_len = (static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[pos])) << 8) |
                             static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[pos + 1]));
        pos += 2;
        // §5.2: the minimal number of bytes MUST be used to encode the length.
        // Accepting a padded encoding would let the same message arrive in two
        // spellings, which is a parser-differential waiting to happen.
        if (header.payload_len < 126) {
            return FrameStatus::kProtocolError;
        }
    } else {
        if (buf.size() < pos + 8) {
            return FrameStatus::kIncomplete;
        }
        std::uint64_t len = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[pos + i]));
        }
        pos += 8;
        // §5.2: the most significant bit MUST be zero.
        if ((len & 0x8000000000000000ULL) != 0) {
            return FrameStatus::kProtocolError;
        }
        if (len < 65536) {
            return FrameStatus::kProtocolError;  // Non-minimal, as above.
        }
        header.payload_len = len;
    }

    // §5.5: control frames must not be fragmented and must be short enough to
    // fit in a single frame. Both are load-bearing — an unbounded "ping" is a
    // memory exhaustion primitive.
    if (is_control(header.opcode)) {
        if (!header.fin || header.payload_len > kMaxControlPayload) {
            return FrameStatus::kProtocolError;
        }
    }

    if (header.masked) {
        if (buf.size() < pos + 4) {
            return FrameStatus::kIncomplete;
        }
        std::memcpy(&header.mask_key, buf.data() + pos, 4);
        pos += 4;
    }

    header.header_size = pos;
    out = header;
    return FrameStatus::kOk;
}

/// XOR a span with the frame's masking key (§5.3).
///
/// `offset` is the payload-relative position of `data[0]`, so a payload split
/// across reads can be unmasked in pieces without buffering it whole.
inline void apply_mask(char* data, std::size_t len, std::uint32_t mask_key,
                       std::size_t offset = 0) noexcept {
    if (mask_key == 0) {
        return;  // XOR with zero is identity; skip the loop entirely.
    }
    unsigned char key[4];
    std::memcpy(key, &mask_key, 4);
    for (std::size_t i = 0; i < len; ++i) {
        data[i] = static_cast<char>(static_cast<unsigned char>(data[i]) ^ key[(i + offset) & 3U]);
    }
}

/// Serialise a frame header into `buf`, which must have room for
/// `kMaxHeaderSize` bytes. Returns the number written.
///
/// A client MUST mask every frame it sends (§5.3), so `mask_key` is required
/// rather than optional. Passing a predictable key is a protocol violation in
/// spirit if not in letter; the caller is expected to supply a random one, and
/// `WebSocketClient` does.
[[nodiscard]] inline std::size_t write_frame_header(char* buf, Opcode opcode,
                                                    std::uint64_t payload_len,
                                                    std::uint32_t mask_key,
                                                    bool fin = true) noexcept {
    std::size_t pos = 0;
    buf[pos++] = static_cast<char>((fin ? 0x80U : 0x00U) | static_cast<std::uint8_t>(opcode));

    if (payload_len < 126) {
        buf[pos++] = static_cast<char>(0x80U | static_cast<std::uint8_t>(payload_len));
    } else if (payload_len <= 0xFFFF) {
        buf[pos++] = static_cast<char>(0x80U | 126U);
        buf[pos++] = static_cast<char>((payload_len >> 8) & 0xFFU);
        buf[pos++] = static_cast<char>(payload_len & 0xFFU);
    } else {
        buf[pos++] = static_cast<char>(0x80U | 127U);
        for (int shift = 56; shift >= 0; shift -= 8) {
            buf[pos++] = static_cast<char>((payload_len >> shift) & 0xFFU);
        }
    }

    std::memcpy(buf + pos, &mask_key, 4);
    pos += 4;
    return pos;
}

/// Close status codes worth naming (§7.4.1).
enum class CloseCode : std::uint16_t {
    kNormal = 1000,
    kGoingAway = 1001,
    kProtocolError = 1002,
    kUnsupportedData = 1003,
    /// Reserved: never sent on the wire, used locally for "closed without one".
    kNoStatus = 1005,
    /// Reserved: connection dropped without a close frame.
    kAbnormal = 1006,
    kInvalidPayload = 1007,
    kPolicyViolation = 1008,
    kMessageTooBig = 1009,
    kInternalError = 1011,
};

/// What `FrameReader::next` produced.
enum class ReadStatus : std::uint8_t {
    /// Nothing complete yet. Read more bytes from the transport.
    kNeedMore,
    /// A complete data message (text or binary), reassembled across fragments.
    kMessage,
    kPing,
    kPong,
    kClose,
    /// The peer violated RFC 6455. Close the connection; do not attempt to
    /// resynchronise.
    kProtocolError,
    /// A message exceeded the configured ceiling. Distinguished from a protocol
    /// error because it is our limit, not the peer's mistake, and it maps to
    /// close code 1009 rather than 1002.
    kMessageTooLarge,
};

/// One decoded event. `payload` is valid until the next call to `next` or
/// `append` on the same reader.
struct Event {
    Opcode opcode{Opcode::kContinuation};
    std::string_view payload;
    /// Only meaningful when the status is kClose. 1005 means the peer sent no
    /// code, which the specification distinguishes from sending 1000.
    std::uint16_t close_code{static_cast<std::uint16_t>(CloseCode::kNoStatus)};
};

/// Incremental frame reader: bytes in, messages out.
///
/// Handles the three things that make this more than a length-prefix loop —
/// fragmentation, control frames interleaved between fragments, and frames that
/// straddle transport reads.
///
/// ZERO-COPY FAST PATH: an unfragmented message that is already fully buffered
/// is returned as a view straight into the receive buffer, with no copy at all.
/// That is the overwhelmingly common case for an exchange feed, where a message
/// is a couple of hundred bytes and arrives whole. Fragmented messages fall back
/// to reassembly into a separate buffer, because there is nowhere contiguous to
/// point at.
class FrameReader {
public:
    /// The default ceiling is generous for a market data feed — the largest
    /// book snapshot from any venue covered here is well under a megabyte — and
    /// finite, which is the property that matters. A reassembly buffer that
    /// grows to whatever the peer asks for is a remote out-of-memory.
    static constexpr std::size_t kDefaultMaxMessage = 8U * 1024U * 1024U;

    explicit FrameReader(std::size_t max_message_bytes = kDefaultMaxMessage)
        : max_message_bytes_(max_message_bytes) {
        buf_.reserve(64 * 1024);
    }

    /// Hand raw transport bytes to the reader.
    void append(const char* data, std::size_t len) {
        compact();
        buf_.insert(buf_.end(), data, data + len);
    }

    void append(std::string_view bytes) { append(bytes.data(), bytes.size()); }

    /// Space the caller can read transport bytes directly into, avoiding a copy
    /// through an intermediate buffer. Follow with `commit`.
    [[nodiscard]] char* writable_tail(std::size_t len) {
        compact();
        const std::size_t old = buf_.size();
        buf_.resize(old + len);
        return buf_.data() + old;
    }

    /// Report how many of the bytes handed out by `writable_tail` were filled.
    void commit(std::size_t written, std::size_t requested) noexcept {
        buf_.resize(buf_.size() - (requested - written));
    }

    /// Pull the next event, if one is complete.
    ///
    /// A failure is LATCHED. Once this has reported a protocol error or an
    /// oversized message, it reports the same thing forever, until `reset`.
    /// There is no resynchronisation point in a stream of length-prefixed
    /// frames: after a bad length the next byte read as an opcode is whatever
    /// happened to be there. A reader that recovered would be inventing frames,
    /// and inventing frames is how a plausible, wrong book gets built.
    [[nodiscard]] ReadStatus next(Event& out) {
        if (failure_ != ReadStatus::kNeedMore) {
            return failure_;
        }

        // The previous event may have pointed into buf_; only now is it safe to
        // drop those bytes.
        consume_pending();

        for (;;) {
            const std::string_view view(buf_.data() + read_pos_, buf_.size() - read_pos_);

            FrameHeader header;
            const FrameStatus status = parse_frame_header(view, header);
            if (status == FrameStatus::kIncomplete) {
                return ReadStatus::kNeedMore;
            }
            if (status == FrameStatus::kProtocolError) {
                return latch(ReadStatus::kProtocolError);
            }

            // §5.1: a server MUST NOT mask. A masked frame from a server means
            // we are not talking to the server we think we are, or we have lost
            // frame alignment. Either way, stop.
            if (header.masked) {
                return latch(ReadStatus::kProtocolError);
            }

            // Reject an oversized frame before waiting for its bytes to arrive:
            // otherwise a declared 4 GiB payload makes us buffer until we die,
            // and the ceiling protects nothing.
            if (header.payload_len > max_message_bytes_ ||
                assembled_.size() + header.payload_len > max_message_bytes_) {
                return latch(ReadStatus::kMessageTooLarge);
            }

            const std::size_t frame_total = header.header_size +
                                            static_cast<std::size_t>(header.payload_len);
            if (view.size() < frame_total) {
                return ReadStatus::kNeedMore;
            }

            const char* payload = view.data() + header.header_size;
            const auto payload_len = static_cast<std::size_t>(header.payload_len);

            if (is_control(header.opcode)) {
                // Control frames are self-contained and never join the message
                // under assembly, so a ping between two fragments leaves the
                // partial message exactly as it was.
                read_pos_ += frame_total;
                return emit_control(header.opcode, payload, payload_len, out);
            }

            // --- Data frame ---

            if (header.opcode == Opcode::kContinuation) {
                if (!assembling_) {
                    return latch(ReadStatus::kProtocolError);  // §5.4: nothing to continue.
                }
            } else {
                if (assembling_) {
                    return latch(ReadStatus::kProtocolError);  // §5.4: interleaved messages.
                }
                message_opcode_ = header.opcode;
            }

            if (header.fin && !assembling_) {
                // Whole message in one frame, already buffered: hand back a view
                // into the receive buffer and defer the consume until the caller
                // has had a chance to read it.
                out.opcode = message_opcode_;
                out.payload = std::string_view(payload, payload_len);
                out.close_code = static_cast<std::uint16_t>(CloseCode::kNoStatus);
                pending_consume_ = frame_total;
                return ReadStatus::kMessage;
            }

            assembled_.insert(assembled_.end(), payload, payload + payload_len);
            assembling_ = true;
            read_pos_ += frame_total;

            if (header.fin) {
                out.opcode = message_opcode_;
                out.payload = std::string_view(assembled_.data(), assembled_.size());
                out.close_code = static_cast<std::uint16_t>(CloseCode::kNoStatus);
                assembling_ = false;
                pending_clear_assembled_ = true;
                return ReadStatus::kMessage;
            }
            // Not the final fragment: loop round for the next frame.
        }
    }

    /// Bytes buffered but not yet consumed. Diagnostic only.
    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - read_pos_; }

    /// True while a fragmented message is partially assembled.
    [[nodiscard]] bool assembling() const noexcept { return assembling_; }

    /// True once a terminal failure has been latched.
    [[nodiscard]] bool failed() const noexcept { return failure_ != ReadStatus::kNeedMore; }

    void reset() noexcept {
        buf_.clear();
        assembled_.clear();
        read_pos_ = 0;
        pending_consume_ = 0;
        pending_clear_assembled_ = false;
        assembling_ = false;
        failure_ = ReadStatus::kNeedMore;
    }

private:
    /// kNeedMore is the "no failure" sentinel: it is the one status `next` can
    /// return that says nothing about the stream's validity.
    [[nodiscard]] ReadStatus latch(ReadStatus status) noexcept {
        failure_ = status;
        return status;
    }

    [[nodiscard]] ReadStatus emit_control(Opcode opcode, const char* payload, std::size_t len,
                                          Event& out) {
        // Copied rather than viewed: control payloads are at most 125 bytes, and
        // copying them means a ping cannot be invalidated by the data frame the
        // caller processes next.
        control_len_ = len;
        if (len > 0) {
            std::memcpy(control_.data(), payload, len);
        }
        out.opcode = opcode;
        out.payload = std::string_view(control_.data(), control_len_);
        out.close_code = static_cast<std::uint16_t>(CloseCode::kNoStatus);

        if (opcode == Opcode::kPing) {
            return ReadStatus::kPing;
        }
        if (opcode == Opcode::kPong) {
            return ReadStatus::kPong;
        }

        // §5.5.1: a close payload is either empty or at least a two-byte code.
        // A single byte is malformed, not "a code we could not read".
        if (len == 1) {
            return ReadStatus::kProtocolError;
        }
        if (len >= 2) {
            out.close_code =
                static_cast<std::uint16_t>((static_cast<std::uint16_t>(
                                                static_cast<unsigned char>(payload[0]))
                                            << 8) |
                                           static_cast<std::uint16_t>(
                                               static_cast<unsigned char>(payload[1])));
            out.payload = std::string_view(control_.data() + 2, control_len_ - 2);
        }
        return ReadStatus::kClose;
    }

    /// Drop the bytes the previously returned event pointed at.
    void consume_pending() noexcept {
        if (pending_consume_ != 0) {
            read_pos_ += pending_consume_;
            pending_consume_ = 0;
        }
        if (pending_clear_assembled_) {
            assembled_.clear();
            pending_clear_assembled_ = false;
        }
    }

    /// Reclaim consumed bytes from the front of the buffer.
    ///
    /// Amortised: the memmove only runs once the consumed prefix is worth
    /// reclaiming, so steady-state reading is not quadratic in message count.
    void compact() {
        consume_pending();
        if (read_pos_ == 0) {
            return;
        }
        if (read_pos_ == buf_.size()) {
            buf_.clear();
            read_pos_ = 0;
            return;
        }
        if (read_pos_ < kCompactThreshold) {
            return;
        }
        const std::size_t remaining = buf_.size() - read_pos_;
        std::memmove(buf_.data(), buf_.data() + read_pos_, remaining);
        buf_.resize(remaining);
        read_pos_ = 0;
    }

    static constexpr std::size_t kCompactThreshold = 32 * 1024;

    std::size_t max_message_bytes_;
    std::vector<char> buf_;
    std::vector<char> assembled_;
    std::array<char, kMaxControlPayload> control_{};
    std::size_t control_len_{0};
    std::size_t read_pos_{0};
    std::size_t pending_consume_{0};
    ReadStatus failure_{ReadStatus::kNeedMore};
    Opcode message_opcode_{Opcode::kText};
    bool pending_clear_assembled_{false};
    bool assembling_{false};
};

}  // namespace crossbook::net
