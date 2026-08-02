// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Kraken WebSocket v2 `book` channel decoder.
//
// Message shape (docs.kraken.com/api/docs/websocket-v2/book):
//
//   {"channel":"book","type":"snapshot"|"update",
//    "data":[{"symbol":"BTC/USD",
//             "bids":[{"price":45283.5,"qty":0.5}, ...],
//             "asks":[{"price":45284.1,"qty":1.2}, ...],
//             "checksum":2418130093,
//             "timestamp":"2026-07-31T12:00:00.000000Z"}]}
//
// Two properties make this the anchor venue:
//
//   - It is public. No API key, so the correctness claim is reproducible by
//     anyone who clones the repository.
//   - Every message carries a CRC32 of the top 10 levels, so the book is
//     verified against the exchange's own arithmetic rather than against
//     itself.
//
// Prices and quantities arrive as JSON *numbers*. They are read as raw tokens
// and converted to exact fixed-point mantissas, never through a double — see
// fixed.hpp for why that is load-bearing rather than fastidious.

#pragma once

#include <cstdint>
#include <string_view>
#include <utility>

#include "crossbook/fixed.hpp"
#include "crossbook/json.hpp"
#include "crossbook/venue.hpp"

namespace crossbook::venues {

namespace detail {

/// Days from 1970-01-01 to y-m-d, proleptic Gregorian. Howard Hinnant's
/// `days_from_civil`, which is exact integer arithmetic over the whole int64
/// range and needs no table, no locale, and no time zone database.
///
/// Written out here rather than reached for via <chrono> because the C++20
/// calendar types are still patchy across the toolchains this header has to
/// compile on, and because a timestamp parser that silently depends on the
/// platform's notion of a clock is not deterministic.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t y, unsigned m,
                                                     unsigned d) noexcept {
    y -= m <= 2;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto yoe = static_cast<unsigned>(y - era * 400);                    // [0, 399]
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;     // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;               // [0, 146096]
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

/// Read exactly `n` digits starting at `pos`, advancing it. Returns false on
/// anything else, so a field of the wrong width is a parse failure rather than
/// a value quietly built from whatever prefix happened to be numeric.
[[nodiscard]] constexpr bool take_digits(std::string_view s, std::size_t& pos, std::size_t n,
                                         unsigned& out) noexcept {
    if (pos + n > s.size()) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    pos += n;
    out = value;
    return true;
}

/// Parse "YYYY-MM-DDTHH:MM:SS[.fraction]Z" to nanoseconds since the Unix epoch.
///
/// Integer-only by construction: a double cannot hold a nanosecond timestamp
/// without loss (2^53 ns is under four months), and the whole library is built
/// on the premise that two processes fed the same bytes cannot disagree.
///
/// The trailing 'Z' is required. Kraken always sends UTC, and an offset form
/// silently read as UTC would shift every staleness decision by hours in the
/// direction that makes a dead feed look fresh, so it is refused instead.
///
/// Fraction digits beyond the ninth are truncated rather than rejected: they are
/// below the resolution the return type can express, and unlike a price a
/// timestamp is never a checksum input, so there is nothing for the extra
/// precision to disagree with.
[[nodiscard]] constexpr bool parse_rfc3339_nanos(std::string_view text,
                                                 std::int64_t& out) noexcept {
    std::size_t pos = 0;
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;

    if (!take_digits(text, pos, 4, year) || pos >= text.size() || text[pos++] != '-' ||
        !take_digits(text, pos, 2, month) || pos >= text.size() || text[pos++] != '-' ||
        !take_digits(text, pos, 2, day) || pos >= text.size() ||
        (text[pos] != 'T' && text[pos] != 't')) {
        return false;
    }
    ++pos;
    if (!take_digits(text, pos, 2, hour) || pos >= text.size() || text[pos++] != ':' ||
        !take_digits(text, pos, 2, minute) || pos >= text.size() || text[pos++] != ':' ||
        !take_digits(text, pos, 2, second)) {
        return false;
    }

    // Range checks. A month of 13 would otherwise produce a plausible-looking
    // instant several weeks away, which is worse than a rejection.
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 ||
        second > 60) {  // 60 is a leap second; the venue may legitimately emit one.
        return false;
    }

    std::int64_t nanos = 0;
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        std::size_t digits = 0;
        std::int64_t scaled = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (digits < 9) {
                scaled = scaled * 10 + (text[pos] - '0');
                ++digits;
            }
            ++pos;
        }
        if (digits == 0) {
            return false;  // A '.' introducing nothing is not a fraction.
        }
        for (std::size_t i = digits; i < 9; ++i) {
            scaled *= 10;
        }
        nanos = scaled;
    }

    if (pos + 1 != text.size() || (text[pos] != 'Z' && text[pos] != 'z')) {
        return false;
    }

    const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), month, day);
    const std::int64_t seconds =
        days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
        static_cast<std::int64_t>(minute) * 60 + static_cast<std::int64_t>(second);
    out = seconds * 1'000'000'000 + nanos;
    return true;
}

}  // namespace detail

/// Decoder for Kraken's v2 book channel.
///
/// Holds the output message so its level buffer is reused across calls; steady
/// state performs no allocation.
class KrakenBookDecoder {
public:
    explicit KrakenBookDecoder(InstrumentSpec spec) : spec_(std::move(spec)) {
        message_.levels.reserve(256);
    }

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    /// Decode one frame. The returned reference is valid until the next call,
    /// and its string views alias `frame`.
    [[nodiscard]] const DecodedMessage& decode(std::string_view frame) {
        message_.reset();

        // Structural validation first: see json::well_formed. Without it a
        // truncated frame decodes as a valid update with the missing side
        // simply absent, and gets applied.
        if (!json::well_formed(frame)) {
            return fail(DecodeError::kMalformed);
        }

        // Venue errors before anything else. Kraken reports a rejected
        // subscription as {"error":"...","method":"subscribe","success":false},
        // which carries no "channel" and so used to fall through to kIgnored —
        // making a pair name we spelled wrong indistinguishable from a market
        // that simply had nothing to say. The feed never synced and nothing
        // anywhere said why.
        const JsonValue error_text = json::find(frame, "error");
        if (error_text && error_text.type == JsonType::kString) {
            return venue_error(json::string_body(error_text));
        }
        const JsonValue success = json::find(frame, "success");
        if (success && success.type == JsonType::kBool && success.raw == "false") {
            return venue_error(json::string_body(json::find(frame, "method")));
        }

        const JsonValue channel = json::find(frame, "channel");
        if (!channel) {
            // Not a channel message at all — an ack, a pong, or a status
            // frame. Not an error; simply not ours.
            message_.kind = MessageKind::kIgnored;
            return message_;
        }
        // Compared through string_equals so a legal escape cannot read as a
        // mismatch. string_body returns empty for an escaped body, which
        // would route a real book frame to kIgnored.
        if (json::string_equals(channel, "status") || json::string_equals(channel, "heartbeat")) {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }
        if (!json::string_equals(channel, "book")) {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }

        const JsonValue type = json::find(frame, "type");
        if (json::string_equals(type, "snapshot")) {
            message_.kind = MessageKind::kSnapshot;
        } else if (json::string_equals(type, "update")) {
            message_.kind = MessageKind::kUpdate;
        } else {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }

        const JsonValue data = json::find(frame, "data");
        if (!data || data.type != JsonType::kArray) {
            return fail(DecodeError::kMalformed);
        }

        // SELECT the entry for our instrument; do not just take the first.
        //
        // The previous code stopped the walk after entry #1 by returning false
        // from the callback — but json::for_each treats an early false as
        // SUCCESS, so entries 2..N were dropped and decode() still reported
        // error == kOk. A multi-instrument frame therefore lost data with no
        // signal at all, and whichever instrument happened to be listed first
        // was applied to this decoder's book regardless of what it was.
        bool matched = false;
        bool failed = false;
        const bool walked = json::for_each(data.raw, [&](const JsonValue& entry) {
            if (entry.type != JsonType::kObject) {
                failed = true;
                return false;
            }
            if (matched) {
                return true;  // Keep walking, so the array is still validated.
            }
            const JsonValue symbol_value = json::find(entry.raw, "symbol");
            std::string_view symbol;
            const json::StringRead read = json::read_string(symbol_value, symbol);
            // An entry with no symbol at all is treated as ours: that is the
            // only reading available, and rejecting it would turn a shape we
            // have never seen into a hard failure on the strength of a guess.
            //
            // An ESCAPED symbol is compared through string_equals rather than
            // by view. "BTC\/USD" is legal JSON for "BTC/USD", and a raw view
            // comparison would call it a foreign instrument and silently drop
            // every frame for the symbol we actually subscribed to.
            const bool present = read != json::StringRead::kNotAString && !symbol.empty();
            if (present && !json::string_equals(symbol_value, spec_.symbol)) {
                return true;  // Some other instrument on a multiplexed socket.
            }
            matched = true;
            failed = !decode_entry(entry.raw);
            return !failed;
        });

        if (!walked || failed) {
            return fail_decoded();
        }
        if (!matched) {
            // The frame was well formed and carried book data, just not for this
            // instrument. Not an error — a multiplexed socket legitimately
            // carries other symbols — but nothing here may touch the book.
            message_.kind = MessageKind::kIgnored;
            message_.levels.clear();
        }
        return message_;
    }

private:
    [[nodiscard]] const DecodedMessage& fail(DecodeError e) noexcept {
        message_.error = e;
        message_.levels.clear();
        return message_;
    }

    /// Finish a decode that went wrong partway through a side.
    ///
    /// Keeps the specific error a level already reported — kPrecisionLoss and
    /// kNonCanonical both name the offending token, and flattening them to
    /// kMalformed would throw that away — but clears the levels either way. A
    /// message that reports an error must not also hand back the levels it read
    /// before hitting it: they came from a frame we have just decided we do not
    /// understand, and fuzz/fuzz_decode.cpp asserts exactly this.
    [[nodiscard]] const DecodedMessage& fail_decoded() noexcept {
        return fail(message_.ok() ? DecodeError::kMalformed : message_.error);
    }

    [[nodiscard]] const DecodedMessage& venue_error(std::string_view detail) noexcept {
        message_.kind = MessageKind::kVenueError;
        message_.bad_token = detail;
        message_.levels.clear();
        return message_;
    }

    [[nodiscard]] bool decode_entry(std::string_view entry) {
        message_.symbol = json::string_body(json::find(entry, "symbol"));

        if (!decode_checksum(entry)) {
            return false;
        }
        if (!decode_timestamp(entry)) {
            return false;
        }
        if (!decode_side(entry, "bids", Side::kBid)) {
            return false;
        }
        if (!decode_side(entry, "asks", Side::kAsk)) {
            return false;
        }
        return true;
    }

    /// Kraken sends the checksum as an unsigned 32-bit value.
    ///
    /// ABSENT is tolerated: verification simply cannot run for that message.
    /// PRESENT BUT UNREADABLE is fatal, and the distinction is the whole point.
    /// Every unreadable spelling used to fall through to has_checksum = false,
    /// which is byte-for-byte the same state as "Kraken sent no checksum" — so
    /// the feed skipped verification, `checksums_verified` never moved, and
    /// `match_rate()` sat at 1.0 having compared nothing. A venue quoting the
    /// field as a string (Kraken already quotes prices as numbers where Binance
    /// quotes strings; venues migrate numeric fields routinely) would have
    /// turned the library's central correctness claim off, silently, forever.
    ///
    /// Both spellings are therefore accepted, and anything else is an error.
    [[nodiscard]] bool decode_checksum(std::string_view entry) {
        const JsonValue checksum = json::find(entry, "checksum");
        if (!checksum) {
            return true;
        }
        const std::string_view token = json::number_token(checksum);
        std::uint64_t raw = 0;
        if (token.empty() || !json::parse_u64(token, raw) || raw > 0xFFFFFFFFULL) {
            message_.error = DecodeError::kBadChecksum;
            message_.bad_token = token.empty() ? checksum.raw : token;
            return false;
        }
        message_.checksum = static_cast<std::uint32_t>(raw);
        message_.has_checksum = true;
        return true;
    }

    /// Kraken stamps every data entry with an RFC3339 instant. Parsing it is
    /// what makes staleness detection exist at all on this venue.
    ///
    /// It was never parsed, so message_.ts stayed 0, so the `last_ts_ == 0`
    /// guard in Feed::check_staleness returned false on every call — the check
    /// was structurally dead on the one venue that publishes checksums, and the
    /// single staleness test used Binance, so nothing noticed.
    ///
    /// A missing or unreadable timestamp is fatal rather than a silent zero:
    /// zero reinstates exactly that dead check, and a feed that cannot go stale
    /// will serve an hour-old book with full confidence.
    [[nodiscard]] bool decode_timestamp(std::string_view entry) {
        const JsonValue timestamp = json::find(entry, "timestamp");
        const std::string_view text = json::string_body(timestamp);
        std::int64_t nanos = 0;
        if (text.empty() || !detail::parse_rfc3339_nanos(text, nanos)) {
            message_.error = DecodeError::kBadTimestamp;
            message_.bad_token = text.empty() ? timestamp.raw : text;
            return false;
        }
        message_.ts = static_cast<Timestamp>(nanos);
        return true;
    }

    [[nodiscard]] bool decode_side(std::string_view entry, std::string_view key, Side side) {
        const JsonValue array = json::find(entry, key);
        if (!array) {
            return true;  // A one-sided update is normal.
        }
        if (array.type != JsonType::kArray) {
            return false;
        }

        bool failed = false;
        const bool walked = json::for_each(array.raw, [&](const JsonValue& level) {
            if (level.type != JsonType::kObject) {
                failed = true;
                return false;
            }
            if (message_.levels.size() >= kMaxLevelsPerMessage) {
                message_.error = DecodeError::kTooManyLevels;
                failed = true;
                return false;
            }

            const std::string_view price_token = json::number_token(json::find(level.raw, "price"));
            const std::string_view qty_token = json::number_token(json::find(level.raw, "qty"));
            if (price_token.empty() || qty_token.empty()) {
                failed = true;
                return false;
            }

            const ParseResult price = parse_fixed(price_token, spec_.price_scale);
            if (!price.ok()) {
                message_.error = (price.error == ParseError::kPrecisionLoss)
                                     ? DecodeError::kPrecisionLoss
                                     : DecodeError::kMalformed;
                message_.bad_token = price_token;
                failed = true;
                return false;
            }
            const ParseResult qty = parse_fixed(qty_token, spec_.qty_scale);
            if (!qty.ok()) {
                message_.error = (qty.error == ParseError::kPrecisionLoss)
                                     ? DecodeError::kPrecisionLoss
                                     : DecodeError::kMalformed;
                message_.bad_token = qty_token;
                failed = true;
                return false;
            }

            // The checksum precondition, checked rather than assumed. Kraken
            // hashes the token as spelled on the wire; the fast path substitutes
            // the mantissa's digits, and those agree only while the venue keeps
            // spelling values canonically at the instrument's scale. If it stops
            // — "0.5" where the scale is 8 — every checksum fails against a book
            // that is numerically perfect. Catching it here names the token;
            // catching it downstream means staring at a match rate of zero.
            if (!is_canonical_at_scale(price_token, spec_.price_scale)) {
                message_.error = DecodeError::kNonCanonical;
                message_.bad_token = price_token;
                failed = true;
                return false;
            }
            if (!is_canonical_at_scale(qty_token, spec_.qty_scale)) {
                message_.error = DecodeError::kNonCanonical;
                message_.bad_token = qty_token;
                failed = true;
                return false;
            }

            message_.levels.push_back(LevelUpdate{side, Price{price.mantissa}, Qty{qty.mantissa}});
            return true;
        });

        return walked && !failed;
    }

    InstrumentSpec spec_;
    DecodedMessage message_;
};

}  // namespace crossbook::venues
