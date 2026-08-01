// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Core value types shared by every venue and every book implementation.

#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>

#include "crossbook/fixed.hpp"

namespace crossbook {

/// Book side. Values chosen so `!side` is a valid opposite-side index.
enum class Side : std::uint8_t { kBid = 0, kAsk = 1 };

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return s == Side::kBid ? Side::kAsk : Side::kBid;
}

[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
    return s == Side::kBid ? "bid" : "ask";
}

/// A price, stored as an exact fixed-point mantissa at the instrument's price
/// scale. Strongly typed so it cannot be confused with a quantity, and with no
/// implicit conversion to any floating point type — the absence of that
/// conversion is the point (see fixed.hpp).
struct Price {
    std::int64_t ticks{0};

    friend constexpr auto operator<=>(const Price&, const Price&) noexcept = default;
    friend constexpr bool operator==(const Price&, const Price&) noexcept = default;
};

/// A quantity, stored as an exact fixed-point mantissa at the instrument's
/// quantity scale.
struct Qty {
    std::int64_t units{0};

    [[nodiscard]] constexpr bool is_zero() const noexcept { return units == 0; }

    friend constexpr auto operator<=>(const Qty&, const Qty&) noexcept = default;
    friend constexpr bool operator==(const Qty&, const Qty&) noexcept = default;
};

/// One aggregated price level.
struct Level {
    Price price{};
    Qty qty{};

    friend constexpr bool operator==(const Level&, const Level&) noexcept = default;
};

/// Per-instrument metadata needed to interpret the wire representation.
///
/// The scales are not cosmetic: they define the mantissa, and the mantissa
/// defines the checksum token. Getting a scale wrong produces a book that is
/// numerically correct but fails every checksum, which is exactly the kind of
/// silent disagreement this library is built to surface.
struct InstrumentSpec {
    std::string symbol;
    Scale price_scale{0};
    Scale qty_scale{0};
};

/// Nanoseconds since the Unix epoch. Venues disagree wildly about their own
/// time formats; normalising early keeps that mess out of the book.
using Timestamp = std::int64_t;

/// A monotonically increasing per-venue update counter, used for gap detection.
/// Distinct venues number these differently — see sequence.hpp.
using SequenceId = std::uint64_t;

}  // namespace crossbook
