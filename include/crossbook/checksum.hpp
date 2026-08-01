// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Kraken WebSocket v2 book checksum — the exchange's own proof that your
// reconstruction is correct.
//
// THE ALGORITHM (docs.kraken.com/api/docs/guides/spot-ws-book-v2):
//
//   1. Take the top 10 ask levels, sorted low price to high.
//   2. For each, take price and qty as they appear on the wire, remove the
//      decimal point, strip leading zeros, and concatenate price then qty.
//   3. Do the same for the top 10 bid levels, sorted high price to low.
//   4. Concatenate the asks string then the bids string.
//   5. CRC32 (IEEE) the result. Compare, as an unsigned 32-bit value, to the
//      `checksum` field on the message.
//
// WHY THIS IS CHEAP HERE:
//
// Step 2 — remove the point, strip leading zeros — is exactly "print the
// fixed-point mantissa in decimal". Because the book already stores mantissas
// (see fixed.hpp), there is no formatting step, no decimal string, and no
// rounding to get wrong. It is an integer-to-ASCII into a stack buffer.
//
// This is the concrete payoff for refusing to store prices as double, and it is
// why the checksum verifier can run on every single update in the hot path
// instead of being sampled.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "crossbook/book.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// Number of levels per side that Kraken's checksum covers, regardless of the
/// depth you subscribed to.
inline constexpr std::size_t kKrakenChecksumDepth = 10;

namespace detail {

/// CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) lookup table, built at
/// compile time so there is no static initialisation order to worry about.
struct Crc32Table {
    std::uint32_t entries[256]{};

    constexpr Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            entries[i] = c;
        }
    }
};

inline constexpr Crc32Table kCrc32Table{};

/// Widest decimal an int64 mantissa can produce, plus room for a sign we never
/// emit. Levels always carry positive price and quantity.
inline constexpr std::size_t kMaxDigits = 20;

/// Write |value| in decimal to `buf`, returning the number of bytes written.
/// No allocation, no std::to_string, no locale.
inline std::size_t write_digits(std::int64_t value, char* buf) noexcept {
    // Magnitude taken in unsigned space: negating INT64_MIN is UB in signed
    // arithmetic. See the same idiom in fixed.hpp::format_fixed.
    const auto bits = static_cast<std::uint64_t>(value);
    std::uint64_t magnitude = (value < 0) ? (0ULL - bits) : bits;
    char tmp[kMaxDigits];
    std::size_t n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
    } while (magnitude != 0);

    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];  // Reverse into place.
    }
    return n;
}

}  // namespace detail

/// Streaming CRC32 (IEEE), so the checksum can be fed level by level without
/// ever materialising the concatenated string.
class Crc32 {
public:
    constexpr Crc32() noexcept = default;

    void update(const char* data, std::size_t len) noexcept {
        std::uint32_t c = state_;
        for (std::size_t i = 0; i < len; ++i) {
            c = detail::kCrc32Table.entries[(c ^ static_cast<unsigned char>(data[i])) & 0xFFU] ^
                (c >> 8);
        }
        state_ = c;
    }

    void update(std::string_view s) noexcept { update(s.data(), s.size()); }

    [[nodiscard]] std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFU; }

    void reset() noexcept { state_ = 0xFFFFFFFFU; }

private:
    std::uint32_t state_{0xFFFFFFFFU};
};

/// CRC32 of a byte string. Free function for tests and known-answer vectors.
[[nodiscard]] inline std::uint32_t crc32(std::string_view s) noexcept {
    Crc32 c;
    c.update(s);
    return c.value();
}

/// Compute Kraken's book checksum over the current state of `book`.
///
/// Allocation-free: each level's digits go into a stack buffer and straight
/// into the CRC. Works with any BasicL2Book instantiation.
template <typename SideImpl>
[[nodiscard]] std::uint32_t kraken_checksum(const BasicL2Book<SideImpl>& book) noexcept {
    Crc32 crc;
    char buf[detail::kMaxDigits * 2];

    // Asks first (low to high), then bids (high to low). Both sides already
    // iterate in book order, which is precisely the order Kraken specifies.
    for (const Side s : {Side::kAsk, Side::kBid}) {
        std::size_t taken = 0;
        book.side(s).for_each([&](const Level& lvl) {
            std::size_t n = detail::write_digits(lvl.price.ticks, buf);
            n += detail::write_digits(lvl.qty.units, buf + n);
            crc.update(buf, n);
            return ++taken < kKrakenChecksumDepth;
        });
    }
    return crc.value();
}

/// The exact string Kraken's checksum is computed over.
///
/// Allocates, and is not used by the verifier. It exists so that when a
/// checksum mismatch is reported, the divergence log can show the input that
/// produced it — a bare "expected X, got Y" is not enough to debug a book, and
/// reconstructing this by hand at 3am is miserable.
template <typename SideImpl>
[[nodiscard]] std::string kraken_checksum_payload(const BasicL2Book<SideImpl>& book) {
    std::string out;
    out.reserve(kKrakenChecksumDepth * 2 * 16);
    for (const Side s : {Side::kAsk, Side::kBid}) {
        std::size_t taken = 0;
        book.side(s).for_each([&](const Level& lvl) {
            char buf[detail::kMaxDigits * 2];
            std::size_t n = detail::write_digits(lvl.price.ticks, buf);
            n += detail::write_digits(lvl.qty.units, buf + n);
            out.append(buf, n);
            return ++taken < kKrakenChecksumDepth;
        });
    }
    return out;
}

}  // namespace crossbook
