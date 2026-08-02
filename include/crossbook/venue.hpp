// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The venue-agnostic decode result.
//
// Every venue speaks a different dialect — different field names, different
// snapshot semantics, numbers as JSON numbers on one venue and JSON strings on
// another, three mutually incompatible ways of expressing sequence continuity.
// All of that is normalised here, once, so the book and the verifier never
// learn a venue's name.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "crossbook/sequence.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// What a decoded message turned out to be.
enum class MessageKind : std::uint8_t {
    /// Not a book message: heartbeat, subscription ack, pong. Ignore it.
    kIgnored,
    /// A full book image. The book must be cleared before applying it.
    kSnapshot,
    /// An incremental update.
    kUpdate,
    /// The venue reported an error.
    kVenueError,
};

/// Why a message could not be decoded.
enum class DecodeError : std::uint8_t {
    kOk,
    /// Not navigable as the venue's documented shape.
    kMalformed,
    /// A price or quantity was not exactly representable at the instrument's
    /// configured scale. Almost always means the venue changed precision.
    kPrecisionLoss,
    /// A sequence field was missing or not an integer.
    kBadSequence,
    /// More levels in one message than the decoder is configured to hold.
    kTooManyLevels,
    /// A price or quantity parsed exactly but was not spelled canonically at
    /// the instrument's scale — "0.5" where the scale is 8. Distinct from
    /// kPrecisionLoss because the value is representable; what is wrong is the
    /// spelling, and Kraken's checksum is computed from the spelling. See
    /// fixed.hpp `is_canonical_at_scale`.
    kNonCanonical,
    /// A checksum field was present but could not be read as a uint32. Never
    /// reported for an absent checksum: absence means verification cannot run
    /// for this message, whereas an unreadable one means the venue changed a
    /// field we verify against and we no longer understand it.
    kBadChecksum,
    /// A timestamp field was missing or not RFC3339. Fatal because a venue whose
    /// timestamps we cannot read has no staleness detection at all, and a feed
    /// that cannot go stale is a feed that will serve an hour-old book.
    kBadTimestamp,
};

[[nodiscard]] constexpr std::string_view to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::kOk:
            return "ok";
        case DecodeError::kMalformed:
            return "malformed";
        case DecodeError::kPrecisionLoss:
            return "precision_loss";
        case DecodeError::kBadSequence:
            return "bad_sequence";
        case DecodeError::kTooManyLevels:
            return "too_many_levels";
        case DecodeError::kNonCanonical:
            return "non_canonical";
        case DecodeError::kBadChecksum:
            return "bad_checksum";
        case DecodeError::kBadTimestamp:
            return "bad_timestamp";
    }
    return "unknown";
}

/// One price level from a decoded message.
struct LevelUpdate {
    Side side{};
    Price price{};
    Qty qty{};
};

/// The normalised result of decoding one websocket frame.
///
/// String members are views into the caller's buffer, so a DecodedMessage is
/// only valid while that buffer lives. The level vector is reused between
/// calls, which is what keeps steady-state decoding allocation-free.
struct DecodedMessage {
    MessageKind kind{MessageKind::kIgnored};
    DecodeError error{DecodeError::kOk};
    std::string_view symbol;
    Timestamp ts{0};

    UpdateIds ids{};
    bool has_ids{false};

    /// Present on Kraken, absent on Binance. The reason verification strategy
    /// is per-venue rather than global.
    std::uint32_t checksum{0};
    bool has_checksum{false};

    std::vector<LevelUpdate> levels;

    /// The offending token, when `error` is kPrecisionLoss or kNonCanonical.
    /// Without it a precision change is an unexplained collapse in match rate.
    ///
    /// Also carries the venue's own error text when `kind` is kVenueError: the
    /// thing that went wrong is then a message rather than a token, but it needs
    /// reporting through the same channel, and a second near-identical field
    /// would only invite setting the wrong one.
    std::string_view bad_token;

    [[nodiscard]] bool ok() const noexcept { return error == DecodeError::kOk; }

    void reset() noexcept {
        kind = MessageKind::kIgnored;
        error = DecodeError::kOk;
        symbol = {};
        ts = 0;
        ids = {};
        has_ids = false;
        checksum = 0;
        has_checksum = false;
        bad_token = {};
        levels.clear();  // Keeps capacity: no allocation after the first message.
    }
};

/// Upper bound on levels accepted from a single message. A venue that sends
/// more than this is either misbehaving or hostile; either way, unbounded
/// growth driven by a socket is not acceptable.
inline constexpr std::size_t kMaxLevelsPerMessage = 10'000;

}  // namespace crossbook
