// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Exact fixed-point decimal arithmetic for order book prices and quantities.
//
// WHY THIS EXISTS (this is not a style preference):
//
// Kraken's WebSocket v2 book channel publishes a CRC32 checksum of the top 10
// levels, computed by taking each level's price and quantity *as they appear on
// the wire*, removing the decimal point, and stripping leading zeros. See
// https://docs.kraken.com/api/docs/guides/spot-ws-book-v2
//
// Worked from the vendor's own example: price 45285.2 at a price scale of 1
// becomes "452852"; quantity 0.00100000 at a quantity scale of 8 becomes
// "100000". Both are exactly the decimal digits of the value's fixed-point
// mantissa at the instrument's scale.
//
// So a book that stores prices as `double` cannot reproduce the exchange's
// checksum without round-tripping back through decimal formatting, which
// reintroduces exactly the rounding it was trying to avoid. Storing the
// mantissa makes checksum token generation a plain integer-to-string, and makes
// two books fed identical messages incapable of disagreeing about whether a
// price level exists.
//
// Everything here is constexpr, allocation-free, and exact. Parsing rejects
// inputs it cannot represent without loss rather than silently rounding.

#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace crossbook {

/// Number of decimal places in a fixed-point value. `value = mantissa * 10^-scale`.
using Scale = std::uint8_t;

/// Largest scale we accept. 10^18 is the most that fits an int64 mantissa, and
/// no venue quotes anywhere near this many decimals.
inline constexpr Scale kMaxScale = 18;

/// Powers of ten, indexed by exponent. Table lookup beats repeated multiply and
/// keeps the hot path branch-free.
inline constexpr std::array<std::int64_t, 19> kPow10 = {
    1LL,
    10LL,
    100LL,
    1'000LL,
    10'000LL,
    100'000LL,
    1'000'000LL,
    10'000'000LL,
    100'000'000LL,
    1'000'000'000LL,
    10'000'000'000LL,
    100'000'000'000LL,
    1'000'000'000'000LL,
    10'000'000'000'000LL,
    100'000'000'000'000LL,
    1'000'000'000'000'000LL,
    10'000'000'000'000'000LL,
    100'000'000'000'000'000LL,
    1'000'000'000'000'000'000LL,
};

/// Why a decimal string could not be converted to a mantissa at a given scale.
enum class ParseError : std::uint8_t {
    kOk = 0,
    kEmpty,          ///< No input.
    kMalformed,      ///< Not a number we recognise.
    kPrecisionLoss,  ///< More significant decimals than the target scale holds.
    kOverflow,       ///< Does not fit in an int64 mantissa.
    kScaleTooLarge,  ///< Requested scale exceeds kMaxScale.
};

/// Result of a parse: a mantissa plus a status. Deliberately not `std::expected`
/// so this header compiles on toolchains that predate it.
struct ParseResult {
    std::int64_t mantissa{0};
    ParseError error{ParseError::kOk};

    [[nodiscard]] constexpr bool ok() const noexcept { return error == ParseError::kOk; }
    explicit constexpr operator bool() const noexcept { return ok(); }
};

namespace detail {

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Multiply with overflow detection. Returns false if the product would wrap.
constexpr bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (a > kMax / b) {
        return false;
    }
    out = a * b;
    return true;
}

/// Add with overflow detection.
constexpr bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (a > kMax - b) {
        return false;
    }
    out = a + b;
    return true;
}

}  // namespace detail

/// Parse a decimal string into a mantissa at `scale`, exactly.
///
/// Accepts an optional leading sign, an integer part, an optional fractional
/// part, and an optional decimal exponent (`1.5e-3`). Venues quote plain
/// decimals in practice, but JSON permits exponent form and the fuzzer will
/// find it, so it is handled rather than rejected.
///
/// Fails with kPrecisionLoss rather than rounding. A book that quietly rounds a
/// price it cannot represent is a book that silently disagrees with the venue,
/// which is the failure mode this whole library exists to make impossible.
///
/// Allocation-free and constexpr-evaluable.
[[nodiscard]] constexpr ParseResult parse_fixed(std::string_view text, Scale scale) noexcept {
    if (scale > kMaxScale) {
        return {0, ParseError::kScaleTooLarge};
    }
    if (text.empty()) {
        return {0, ParseError::kEmpty};
    }

    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = (text[i] == '-');
        ++i;
    }

    std::int64_t digits = 0;      // All significant digits seen, as an integer.
    int fractional_count = 0;     // How many of those came after the '.'.
    bool any_digit = false;
    bool overflowed = false;

    // Integer part.
    for (; i < text.size() && detail::is_digit(text[i]); ++i) {
        any_digit = true;
        if (!overflowed) {
            std::int64_t scaled = 0;
            if (!detail::checked_mul(digits, 10, scaled) ||
                !detail::checked_add(scaled, static_cast<std::int64_t>(text[i] - '0'), digits)) {
                overflowed = true;
            }
        }
    }

    // Fractional part.
    if (i < text.size() && text[i] == '.') {
        ++i;
        for (; i < text.size() && detail::is_digit(text[i]); ++i) {
            any_digit = true;
            ++fractional_count;
            if (!overflowed) {
                std::int64_t scaled = 0;
                if (!detail::checked_mul(digits, 10, scaled) ||
                    !detail::checked_add(scaled, static_cast<std::int64_t>(text[i] - '0'), digits)) {
                    overflowed = true;
                }
            }
        }
    }

    if (!any_digit) {
        return {0, ParseError::kMalformed};
    }

    // Optional exponent.
    int exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exp_negative = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exp_negative = (text[i] == '-');
            ++i;
        }
        if (i >= text.size() || !detail::is_digit(text[i])) {
            return {0, ParseError::kMalformed};
        }
        for (; i < text.size() && detail::is_digit(text[i]); ++i) {
            exponent = exponent * 10 + (text[i] - '0');
            if (exponent > 1000) {  // Absurd; clamp so we cannot overflow the int.
                exponent = 1000;
            }
        }
        if (exp_negative) {
            exponent = -exponent;
        }
    }

    if (i != text.size()) {
        return {0, ParseError::kMalformed};  // Trailing garbage.
    }
    if (overflowed) {
        return {0, ParseError::kOverflow};
    }

    // The value is `digits * 10^(exponent - fractional_count)`. We want it
    // expressed as `mantissa * 10^-scale`, so shift by this much:
    const int shift = static_cast<int>(scale) + exponent - fractional_count;

    std::int64_t mantissa = digits;
    if (shift > 0) {
        if (shift > static_cast<int>(kMaxScale)) {
            return {0, ParseError::kOverflow};
        }
        if (!detail::checked_mul(mantissa, kPow10[static_cast<std::size_t>(shift)], mantissa)) {
            return {0, ParseError::kOverflow};
        }
    } else if (shift < 0) {
        const int drop = -shift;
        if (drop > static_cast<int>(kMaxScale)) {
            // Everything significant would be shifted away. Only exact if zero.
            return digits == 0 ? ParseResult{0, ParseError::kOk}
                               : ParseResult{0, ParseError::kPrecisionLoss};
        }
        const std::int64_t divisor = kPow10[static_cast<std::size_t>(drop)];
        if (mantissa % divisor != 0) {
            return {0, ParseError::kPrecisionLoss};  // Would need to round. Refuse.
        }
        mantissa /= divisor;
    }

    return {negative ? -mantissa : mantissa, ParseError::kOk};
}

/// Render a mantissa back to its canonical decimal string at `scale`, with
/// trailing zeros preserved (0.001 at scale 8 renders as "0.00100000").
///
/// Round-tripping through this is how the decoder proves it understood the
/// wire representation: see `round_trips()`.
[[nodiscard]] inline std::string format_fixed(std::int64_t mantissa, Scale scale) {
    const bool negative = mantissa < 0;
    // Negating INT64_MIN in signed arithmetic is UB, so take the magnitude in
    // unsigned space where wraparound is defined. C++20 fixes two's complement
    // representation, making the conversion exact rather than implementation
    // defined.
    const auto bits = static_cast<std::uint64_t>(mantissa);
    const std::uint64_t magnitude = negative ? (0ULL - bits) : bits;
    const auto width = static_cast<std::size_t>(scale);

    std::string digits = std::to_string(magnitude);
    if (width > 0) {
        if (digits.size() <= width) {
            digits.insert(0, width + 1 - digits.size(), '0');
        }
        digits.insert(digits.size() - width, 1, '.');
    }
    if (negative) {
        digits.insert(0, 1, '-');
    }
    return digits;
}

/// True if `text` is exactly representable at `scale` and formats back to an
/// equivalent decimal value.
///
/// Used on ingest to turn an assumption ("the venue always quotes at the
/// instrument's documented precision") into a checked invariant. When a venue
/// changes precision without announcing it, this is what notices.
[[nodiscard]] inline bool round_trips(std::string_view text, Scale scale) {
    const ParseResult parsed = parse_fixed(text, scale);
    if (!parsed.ok()) {
        return false;
    }
    // Compare numerically rather than byte-wise: "45285.20" and "45285.2" are
    // the same value, and only the checksum path cares about the exact spelling.
    const ParseResult reparsed = parse_fixed(format_fixed(parsed.mantissa, scale), scale);
    return reparsed.ok() && reparsed.mantissa == parsed.mantissa;
}

/// The checksum token for a fixed-point value, per Kraken's algorithm: the
/// canonical decimal with its point removed and leading zeros stripped.
///
/// As documented at the top of this file, that is precisely the mantissa's
/// decimal digits, so this is an integer-to-string and nothing more. Kept as a
/// named function because the *reason* it is this simple is the interesting part.
///
/// PRECONDITION — the one assumption the fast checksum path depends on:
///
/// Kraken computes the checksum from the value *as it appears on the wire*, so
/// substituting the mantissa is equivalent only when the venue spells the value
/// canonically at the instrument's scale, trailing zeros included. Kraken does:
/// its documented quantity example is "0.00100000", padded to the 8-decimal
/// quantity precision.
///
/// If a venue ever sent "0.5" where the scale is 8, the wire transformation
/// gives "5" and this gives "50000000" — every checksum would fail against a
/// book that is numerically perfect, which is a miserable thing to debug from
/// the symptom alone.
///
/// `round_trips()` is the guard: run it on ingest and a precision change shows
/// up as a kPrecisionLoss divergence naming the offending token, instead of as
/// an unexplained collapse in match rate. Pinned by
/// tests/test_fixed.cpp "the checksum-token identity requires canonical wire
/// spelling".
[[nodiscard]] inline std::string checksum_token(std::int64_t mantissa) {
    return std::to_string(mantissa < 0 ? -mantissa : mantissa);
}

}  // namespace crossbook
