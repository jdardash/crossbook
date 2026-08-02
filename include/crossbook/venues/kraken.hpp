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

        // One member walk collects every top-level field this decoder reads.
        // This replaces a well_formed() pre-pass plus five find() calls that
        // each restarted from the front of the frame — the walk validates the
        // whole frame to full depth as it goes (see json::for_each_member),
        // so a truncated frame still cannot decode as a valid update with the
        // missing side simply absent. First occurrence wins, exactly as
        // find() answered, so a duplicate-key frame reads identically through
        // either path.
        JsonValue error_text;
        JsonValue success;
        JsonValue method;
        JsonValue channel;
        JsonValue type;
        JsonValue data;
        const bool walked_frame = json::for_each_member(frame, [&](std::string_view key,
                                                                   const JsonValue& v) {
            if (key == "error") {
                capture(error_text, v);
            } else if (key == "success") {
                capture(success, v);
            } else if (key == "method") {
                capture(method, v);
            } else if (key == "channel") {
                capture(channel, v);
            } else if (key == "type") {
                capture(type, v);
            } else if (key == "data") {
                capture(data, v);
            }
            return true;  // Walk everything: completing the walk IS the
                          // structural validation of the frame.
        });
        if (!walked_frame) {
            // Not a well-formed object. A frame that is valid JSON but not an
            // object — a bare array, a string, a number — was never an error
            // before this rewrite and must not become one: find() simply had
            // nothing to find and the frame fell through to kIgnored. Only
            // genuinely malformed input is rejected.
            if (json::well_formed(frame)) {
                message_.kind = MessageKind::kIgnored;
                return message_;
            }
            return fail(DecodeError::kMalformed);
        }

        // Venue errors before anything else. Kraken reports a rejected
        // subscription as {"error":"...","method":"subscribe","success":false},
        // which carries no "channel" and so used to fall through to kIgnored —
        // making a pair name we spelled wrong indistinguishable from a market
        // that simply had nothing to say. The feed never synced and nothing
        // anywhere said why.
        if (error_text && error_text.type == JsonType::kString) {
            return venue_error(json::string_body(error_text));
        }
        if (success && success.type == JsonType::kBool && success.raw == "false") {
            return venue_error(json::string_body(method));
        }

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

        if (json::string_equals(type, "snapshot")) {
            message_.kind = MessageKind::kSnapshot;
        } else if (json::string_equals(type, "update")) {
            message_.kind = MessageKind::kUpdate;
        } else {
            message_.kind = MessageKind::kIgnored;
            return message_;
        }

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

            // One walk per entry, capturing the five fields this decoder
            // reads. The old shape re-found each of them, and because Kraken
            // spells checksum and timestamp AFTER the level arrays on the
            // wire, each of those finds re-walked both arrays to get there.
            JsonValue symbol_value;
            JsonValue checksum;
            JsonValue timestamp;
            JsonValue bids;
            JsonValue asks;
            if (!json::for_each_member(entry.raw, [&](std::string_view key, const JsonValue& v) {
                    if (key == "symbol") {
                        capture(symbol_value, v);
                    } else if (key == "checksum") {
                        capture(checksum, v);
                    } else if (key == "timestamp") {
                        capture(timestamp, v);
                    } else if (key == "bids") {
                        capture(bids, v);
                    } else if (key == "asks") {
                        capture(asks, v);
                    }
                    return true;
                })) {
                failed = true;
                return false;
            }

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
            failed = !decode_entry(symbol_value, checksum, timestamp, bids, asks);
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

    /// Capture the first occurrence of a key, reproducing find()'s
    /// first-wins duplicate policy through the single-walk path. See the
    /// duplicate-keys note on json::find for why first-wins is the safe
    /// answer; tests/test_json.cpp pins the two paths agreeing.
    static void capture(JsonValue& slot, const JsonValue& v) noexcept {
        if (!slot.ok()) {
            slot = v;
        }
    }

    [[nodiscard]] bool decode_entry(const JsonValue& symbol, const JsonValue& checksum,
                                    const JsonValue& timestamp, const JsonValue& bids,
                                    const JsonValue& asks) {
        message_.symbol = json::string_body(symbol);

        if (!decode_checksum(checksum)) {
            return false;
        }
        if (!decode_timestamp(timestamp)) {
            return false;
        }
        if (!decode_side(bids, Side::kBid)) {
            return false;
        }
        if (!decode_side(asks, Side::kAsk)) {
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
    [[nodiscard]] bool decode_checksum(const JsonValue& checksum) {
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
    [[nodiscard]] bool decode_timestamp(const JsonValue& timestamp) {
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

    [[nodiscard]] bool decode_side(const JsonValue& array, Side side) {
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

            // One walk per level object, not one find() per field. A level is
            // small, but there are up to twenty of them per frame and this is
            // the innermost loop of the whole library.
            JsonValue price_value;
            JsonValue qty_value;
            if (!json::for_each_member(level.raw, [&](std::string_view key, const JsonValue& v) {
                    if (key == "price") {
                        capture(price_value, v);
                    } else if (key == "qty") {
                        capture(qty_value, v);
                    }
                    return true;
                })) {
                failed = true;
                return false;
            }
            const std::string_view price_token = json::number_token(price_value);
            const std::string_view qty_token = json::number_token(qty_value);
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
            if (!is_canonical_at_scale(price_token, spec_.price_scale, price.mantissa)) {
                message_.error = DecodeError::kNonCanonical;
                message_.bad_token = price_token;
                failed = true;
                return false;
            }
            if (!is_canonical_at_scale(qty_token, spec_.qty_scale, qty.mantissa)) {
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
