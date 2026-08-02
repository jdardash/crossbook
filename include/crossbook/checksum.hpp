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

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "crossbook/book.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// Number of levels per side that Kraken's checksum covers, regardless of the
/// depth you subscribed to.
inline constexpr std::size_t kKrakenChecksumDepth = 10;

namespace detail {

/// CRC32 (IEEE 802.3, reflected, poly 0xEDB88320) lookup tables, built at
/// compile time so there is no static initialisation order to worry about.
///
/// Eight tables, not one. A single-table CRC carries a loop dependency through
/// the state with a table load on every byte, which caps it near one byte per
/// three cycles. Slice-by-8 (Kounavis & Berry, Intel 2008 — the same scheme
/// zlib and Linux use for this exact polynomial) folds eight bytes per
/// iteration through eight independent table lookups that the CPU can issue in
/// parallel, because table k is table 0 advanced by k zero bytes:
///
///     entries[k+1][i] == (entries[k][i] >> 8) ^ entries[0][entries[k][i] & 0xFF]
///
/// Hardware CRC instructions are NOT an option here and the reason is worth
/// recording: SSE4.2's crc32 instruction implements CRC32C (poly 0x1EDC6F41),
/// a different polynomial, and Kraken checksums with IEEE 0xEDB88320. PCLMUL
/// folding does handle arbitrary polynomials but only pays above a few hundred
/// bytes, and the payload here is ~240 — inside slice-by-8's home turf. 8 KiB
/// of tables against the 1 KiB the single table cost is the whole price.
struct Crc32Table {
    std::uint32_t entries[8][256]{};

    constexpr Crc32Table() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
            }
            entries[0][i] = c;
        }
        for (std::size_t t = 1; t < 8; ++t) {
            for (std::uint32_t i = 0; i < 256; ++i) {
                const std::uint32_t prev = entries[t - 1][i];
                entries[t][i] = (prev >> 8) ^ entries[0][prev & 0xFFU];
            }
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
        const auto& t = detail::kCrc32Table.entries;

        // Slice-by-8 body. The little-endian requirement is real: the u32
        // loads below place byte 0 in the low lane, and the table indices are
        // derived from lane positions. Every platform this library targets is
        // little-endian; a big-endian port takes the bytewise tail below,
        // which is correct everywhere, just slower.
        if constexpr (std::endian::native == std::endian::little) {
            while (len >= 8) {
                std::uint32_t lo = 0;
                std::uint32_t hi = 0;
                std::memcpy(&lo, data, 4);      // memcpy, not a cast: the data
                std::memcpy(&hi, data + 4, 4);  // is char* and rarely aligned.
                const std::uint32_t one = c ^ lo;
                c = t[7][one & 0xFFU] ^ t[6][(one >> 8) & 0xFFU] ^ t[5][(one >> 16) & 0xFFU] ^
                    t[4][one >> 24] ^ t[3][hi & 0xFFU] ^ t[2][(hi >> 8) & 0xFFU] ^
                    t[1][(hi >> 16) & 0xFFU] ^ t[0][hi >> 24];
                data += 8;
                len -= 8;
            }
        }

        for (std::size_t i = 0; i < len; ++i) {
            c = t[0][(c ^ static_cast<unsigned char>(data[i])) & 0xFFU] ^ (c >> 8);
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

namespace detail {

/// Upper bound on the checksum payload: twenty levels, each a price and a
/// quantity of at most kMaxDigits digits. Real Kraken payloads run ~240 bytes;
/// the bound exists so the buffer below is provably sufficient, not typical.
inline constexpr std::size_t kMaxChecksumPayload = kKrakenChecksumDepth * 2 * kMaxDigits * 2;

}  // namespace detail

/// Kraken's checksum payload, written into a caller-owned buffer of at least
/// `detail::kMaxChecksumPayload` bytes. Returns the number of bytes written.
///
/// This is the one place the payload's byte layout is defined. The verifier
/// CRCs these bytes and the divergence log prints them, through this same
/// function, so the two can never drift apart — a mismatch report always shows
/// exactly the bytes the failing checksum was computed over.
///
/// Asks first (low to high), then bids (high to low). Both sides already
/// iterate in book order, which is precisely the order Kraken specifies.
template <typename SideImpl>
[[nodiscard]] std::size_t kraken_checksum_payload_into(const BasicL2Book<SideImpl>& book,
                                                       char* buf) noexcept {
    std::size_t n = 0;
    for (const Side s : {Side::kAsk, Side::kBid}) {
        std::size_t taken = 0;
        book.side(s).for_each([&](const Level& lvl) {
            n += detail::write_digits(lvl.price.ticks, buf + n);
            n += detail::write_digits(lvl.qty.units, buf + n);
            return ++taken < kKrakenChecksumDepth;
        });
    }
    return n;
}

/// Compute Kraken's book checksum over the current state of `book`.
///
/// Allocation-free: the whole payload goes into one stack buffer and through
/// the CRC in a single pass. Buffering first is not cosmetic — feeding the CRC
/// per level hands it ~12-byte fragments, and slice-by-8 spends most of each
/// fragment in its bytewise tail. One contiguous run keeps the eight-byte body
/// loop fed for ~30 iterations instead of entering it twenty times.
template <typename SideImpl>
[[nodiscard]] std::uint32_t kraken_checksum(const BasicL2Book<SideImpl>& book) noexcept {
    char buf[detail::kMaxChecksumPayload];
    const std::size_t n = kraken_checksum_payload_into(book, buf);
    Crc32 crc;
    crc.update(buf, n);
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
    char buf[detail::kMaxChecksumPayload];
    const std::size_t n = kraken_checksum_payload_into(book, buf);
    return std::string(buf, n);
}

}  // namespace crossbook
