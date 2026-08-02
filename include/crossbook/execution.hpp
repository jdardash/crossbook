// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Executable size: what you can actually trade, as opposed to what the touch
// claims.
//
// WHY THIS IS THE POINT OF THE WHOLE LIBRARY
//
// A quote is not a size. "Best bid 45283.5" says nothing about whether you can
// sell one coin there or a hundred, and sizing a position off the touch is the
// single most common way a spread that looked profitable turns out not to be.
// The gap is not subtle: on a thin book the second level can be basis points
// away, so a trade sized off the touch pays several times the spread it was
// trying to capture.
//
// This is also the concrete reason a *correct* book matters rather than an
// approximately correct one. Depth beyond the touch is exactly the part a
// reconstruction bug corrupts silently — the touch is refreshed constantly and
// self-corrects, while a stale level ten deep can sit there wrong for hours.
// Verifying against the exchange's own checksum is what makes the answer below
// trustworthy.
//
// EVERYTHING HERE IS INTEGER ARITHMETIC. No floats, for the same reason the
// book has none: two runs over the same book must produce identical answers.
//
// WHICH WAY THE INTEGERS ROUND IS A RISK DECISION, NOT A DETAIL
//
// Integer division truncates toward zero, and an earlier version of this file
// let it. The VWAP of a buy therefore came back below the true cost — every
// walk understated what the trade would pay, on every venue, always in the
// trader's favour. That is the one direction a risk-facing number must never
// round: it makes a book look cheaper than it is, it makes a market look more
// crossed than it is, and because it collapses distinct real costs onto the
// same integer it manufactures ties between venues that are not actually tied.
// Every rounding below is therefore pinned to the direction that costs the
// taker money: up when buying, down when selling.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "crossbook/book.hpp"
#include "crossbook/types.hpp"

namespace crossbook {

/// Hundredths of a basis point. 1'000'000 units = 100%.
///
/// WHY NOT PLAIN BASIS POINTS:
///
/// Crypto spreads are routinely well under one basis point. A ten-tick spread
/// on a 45283.5 price is 0.22 bps, which in integer basis points truncates to
/// zero — so an ordinary healthy book reports a spread of nothing, and any
/// comparison built on that silently degrades to "all venues are identical".
/// An earlier version of this file used bps and did exactly that: it reported
/// a real two-level slippage as 0, and declared an uncrossed market crossed.
///
/// 0.01 bps of resolution is enough for every venue and instrument in scope,
/// and integer arithmetic keeps the answers reproducible.
using CentiBps = std::int64_t;

/// Convert whole basis points to the internal unit, for callers who think in
/// bps (which is most of them).
[[nodiscard]] constexpr CentiBps from_bps(std::int64_t bps) noexcept { return bps * 100; }

/// Convert whole percent to the internal unit.
[[nodiscard]] constexpr CentiBps from_percent(std::int64_t pct) noexcept {
    return pct * 10'000;
}

/// Why a walk produced the numbers it did — or why it produced none.
///
/// `Execution{}` used to be the answer to three different questions: an empty
/// book, an overflowing book, and a price limit that excluded every level. All
/// three arrived carrying `qty == 0` *and* `depth_exhausted == false`, which
/// reads as "this venue has depth, the request simply asked for none" — an
/// assertion about the book that two of those three paths had no basis
/// whatsoever for making. Downstream, `best_execution` dropped such a venue
/// with a bare `if (execution.empty()) continue;`, so an overflowing book and a
/// wrapped price limit both left the routing decision as a silent absence.
/// Booleans cannot carry that distinction; an enum can.
enum class ExecutionStatus : std::uint8_t {
    /// Never set by a walk. A default-constructed `Execution` carries this so
    /// that "nothing produced this value" cannot be mistaken for "a walk ran
    /// and succeeded", which is the exact ambiguity this enum exists to remove.
    /// Seeing kUnset escape from a function in this header is a bug in it.
    kUnset = 0,
    /// The walk ran and the numbers below are meaningful.
    kOk,
    /// The side has no levels at all. Nothing about depth is being claimed.
    kEmptyBook,
    /// A notional, a quantity total, or the price limit itself would have
    /// wrapped. A confidently wrong number is worse than no number.
    kOverflow,
    /// The price limit excluded every level, including the touch. Reachable
    /// only from a degenerate book or limit; distinguished because it means
    /// "widen the limit", which is the opposite advice from kEmptyBook.
    kLimitExcludedAll,
    /// The request itself was not answerable: a non-positive size, or a
    /// negative slippage budget. Previously both were accepted and answered
    /// with a confident zero.
    kInvalidRequest,
};

/// The result of walking a book to a price limit.
struct Execution {
    /// Total quantity available within the limit.
    Qty qty{};

    /// Volume-weighted average price, at the instrument's price scale, rounded
    /// in the direction that costs the taker money (up when taking asks, down
    /// when taking bids). Zero when `qty` is zero.
    Price vwap{};

    /// The worst price touched. This, not the VWAP, is what a limit order needs.
    Price limit_price{};

    /// How many price levels were consumed.
    std::size_t levels{0};

    /// True if the walk ran out of book before reaching the price limit — the
    /// venue simply does not have more depth. Distinguishing this from "the
    /// limit stopped us" matters: one means widen the limit, the other means
    /// find another venue.
    ///
    /// Meaningful only when `status` is kOk or kEmptyBook. On the failure
    /// statuses it carries no information, which is precisely why `status`
    /// exists — reading this flag alone on an overflowing book previously
    /// yielded the claim "depth is fine".
    bool depth_exhausted{false};

    /// Slippage of the VWAP against the touch, in hundredths of a basis point.
    /// The number a carry trade's edge has to survive.
    CentiBps slippage{0};

    /// Why the numbers above are what they are. See ExecutionStatus.
    ExecutionStatus status{ExecutionStatus::kUnset};

    [[nodiscard]] bool empty() const noexcept { return qty.units == 0; }

    /// True only when a walk actually ran and produced a tradeable answer.
    /// Prefer this to `!empty()`: the two differ on exactly the paths that
    /// used to lie.
    [[nodiscard]] bool ok() const noexcept { return status == ExecutionStatus::kOk; }
};

namespace detail {

/// Multiply with overflow detection, correct for negative operands.
///
/// `fixed.hpp`'s `checked_mul` tests `a > kMax / b`, which is a valid overflow
/// test only while both operands are non-negative: with a negative `b` the
/// division truncates the other way and the inequality flips sense, so a
/// product that wraps is waved through. Prices are non-negative on every venue
/// covered here, but a fee schedule, a spread delta and a price-limit offset
/// are all reachable from the public API with either sign, and "the caller
/// promised it was positive" is the unchecked precondition this file exists to
/// remove. So magnitudes go through the shared helper and the sign is reapplied
/// here, with the magnitudes taken through uint64 because negating INT64_MIN in
/// signed arithmetic is undefined.
///
/// Conservative at one value: a product of exactly INT64_MIN is reported as
/// overflow, because the magnitude check runs against INT64_MAX. Refusing to
/// answer at the extreme edge of the range is the safe side of that trade.
[[nodiscard]] constexpr std::uint64_t magnitude_u64(std::int64_t v) noexcept {
    const auto bits = static_cast<std::uint64_t>(v);
    return v < 0 ? (0ULL - bits) : bits;
}

[[nodiscard]] constexpr bool checked_mul_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
    constexpr auto kMaxMagnitude =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    const std::uint64_t ua = magnitude_u64(a);
    const std::uint64_t ub = magnitude_u64(b);
    if (ua > kMaxMagnitude || ub > kMaxMagnitude) {
        return false;  // INT64_MIN; its magnitude is not representable.
    }
    std::int64_t product = 0;
    if (!checked_mul(static_cast<std::int64_t>(ua), static_cast<std::int64_t>(ub), product)) {
        return false;
    }
    out = ((a < 0) != (b < 0)) ? -product : product;
    return true;
}

/// Add with overflow detection, correct for negative operands.
///
/// `checked_add` only tests the positive end (`a > kMax - b`), which is exact
/// while `b >= 0` and blind to underflow otherwise. Delegates for the half it
/// gets right.
[[nodiscard]] constexpr bool checked_add_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
    if (b >= 0) {
        return checked_add(a, b, out);
    }
    if (a < (std::numeric_limits<std::int64_t>::min)() - b) {
        return false;
    }
    out = a + b;
    return true;
}

/// Subtract with overflow detection, correct for negative operands.
///
/// Not `checked_add_signed(a, -b, out)`: negating INT64_MIN is undefined, so
/// the one input that most needs a guard is the one that would trip on the way
/// into it.
[[nodiscard]] constexpr bool checked_sub_signed(std::int64_t a, std::int64_t b,
                                                std::int64_t& out) noexcept {
    if (b == (std::numeric_limits<std::int64_t>::min)()) {
        if (a >= 0) {
            return false;  // a + 2^63 does not fit.
        }
        out = a - b;
        return true;
    }
    return checked_add_signed(a, -b, out);
}

/// Integer division rounding toward negative infinity.
[[nodiscard]] constexpr std::int64_t floor_div(std::int64_t num, std::int64_t den) noexcept {
    const std::int64_t quotient = num / den;
    const std::int64_t remainder = num % den;
    return (remainder != 0 && ((remainder < 0) != (den < 0))) ? quotient - 1 : quotient;
}

/// Integer division rounding toward positive infinity.
[[nodiscard]] constexpr std::int64_t ceil_div(std::int64_t num, std::int64_t den) noexcept {
    const std::int64_t quotient = num / den;
    const std::int64_t remainder = num % den;
    return (remainder != 0 && ((remainder < 0) == (den < 0))) ? quotient + 1 : quotient;
}

/// Integer division rounding the magnitude up — away from zero on either sign.
///
/// This is the rounding a cost adjustment needs: whichever way the sign points,
/// the adjustment gets bigger, never smaller. Truncation here is what let a
/// venue's fee be understated by up to a tick on the ask and its proceeds
/// overstated by up to a tick on the bid, both times flattering the venue.
[[nodiscard]] constexpr std::int64_t div_away_from_zero(std::int64_t num,
                                                        std::int64_t den) noexcept {
    const std::int64_t quotient = num / den;
    if (num % den == 0) {
        return quotient;
    }
    return ((num < 0) == (den < 0)) ? quotient + 1 : quotient - 1;
}

/// A VWAP rounded against the taker: up when buying, down when selling.
///
/// `qty` must be non-zero; callers check.
[[nodiscard]] constexpr std::int64_t conservative_vwap(std::int64_t notional, std::int64_t qty,
                                                       Side side) noexcept {
    return side == Side::kAsk ? ceil_div(notional, qty) : floor_div(notional, qty);
}

/// The price limit `slippage` away from `touch`, in the direction that costs
/// the taker money: buying walks up the asks, selling walks down the bids.
///
/// Returns false rather than a wrapped limit. The multiply used to sit
/// unguarded *upstream* of the accumulation loop's overflow guard, so a large
/// budget on a high-priced instrument wrapped the limit negative, the loop then
/// rejected every level as beyond it, and the caller received qty 0 with
/// `depth_exhausted == false` — "you asked for a wide slippage budget on a full
/// book, got nothing, and the flag says depth is fine". The guard the header
/// promised existed; it was just downstream of the thing that wrapped.
[[nodiscard]] constexpr bool price_limit(std::int64_t touch, Side side, CentiBps slippage,
                                         std::int64_t& out) noexcept {
    std::int64_t product = 0;
    if (!checked_mul_signed(touch, slippage, product)) {
        return false;
    }
    // Rounded toward the touch so the limit is never more permissive than asked.
    const std::int64_t offset = product / 1'000'000;
    return side == Side::kAsk ? checked_add_signed(touch, offset, out)
                              : checked_add_signed(touch, -offset, out);
}

/// Distance from `reference` to `moved`, in hundredths of a basis point.
///
/// Guards the multiply: a pathological price can make `delta * 1'000'000`
/// overflow, and a wrapped slippage figure would be worse than no figure.
[[nodiscard]] constexpr CentiBps bps_between(std::int64_t reference,
                                             std::int64_t moved) noexcept {
    if (reference == 0) {
        return 0;
    }
    const std::int64_t delta = moved > reference ? moved - reference : reference - moved;
    std::int64_t scaled = 0;
    if (!checked_mul_signed(delta, 1'000'000, scaled)) {
        return 0;
    }
    return scaled / reference;
}

}  // namespace detail

/// Walk `side` of `book` from the touch, accumulating quantity until the price
/// moves more than `max_slippage` away from it.
///
/// `side` is the side being *taken*: kAsk to buy, kBid to sell.
///
/// Passing `max_slippage == 0` returns only what rests at the touch, which
/// is the honest version of the number most dashboards print as "liquidity".
/// A negative `max_slippage` is rejected outright rather than quietly producing
/// an inverted limit that excludes the whole book and reports it as zero size.
template <typename SideImpl>
[[nodiscard]] Execution executable_size(const BasicL2Book<SideImpl>& book, Side side,
                                        CentiBps max_slippage) {
    Execution result;

    if (max_slippage < 0) {
        result.status = ExecutionStatus::kInvalidRequest;
        return result;
    }

    Level touch{};
    if (!book.best(side, touch)) {
        result.depth_exhausted = true;
        result.status = ExecutionStatus::kEmptyBook;
        return result;
    }

    std::int64_t limit = 0;
    if (!detail::price_limit(touch.price.ticks, side, max_slippage, limit)) {
        result.status = ExecutionStatus::kOverflow;
        return result;
    }

    // Accumulated in the product space (price_ticks * qty_units). At realistic
    // crypto scales — a ~4.5e5 price mantissa and a ~1e8 quantity mantissa —
    // each level contributes ~4.5e13, so hundreds of levels stay far inside
    // int64. The guard below covers the pathological book anyway, because
    // silently wrapping a notional would produce a confidently wrong VWAP.
    std::int64_t notional = 0;
    std::int64_t total_qty = 0;
    bool overflowed = false;
    bool stopped_at_limit = false;

    book.side(side).for_each([&](const Level& level) {
        // Past the limit: stop. The level is not partially taken, because a
        // price level is filled at its price or not at all.
        const bool beyond =
            (side == Side::kAsk) ? (level.price.ticks > limit) : (level.price.ticks < limit);
        if (beyond) {
            stopped_at_limit = true;
            return false;
        }

        std::int64_t contribution = 0;
        if (!detail::checked_mul_signed(level.price.ticks, level.qty.units, contribution) ||
            !detail::checked_add_signed(notional, contribution, notional) ||
            !detail::checked_add_signed(total_qty, level.qty.units, total_qty)) {
            overflowed = true;
            return false;
        }

        result.limit_price = level.price;
        ++result.levels;
        return true;
    });

    if (overflowed) {
        // An overflowing book is not a book anyone should trade against, and
        // reporting a wrapped number would be worse than reporting nothing.
        Execution failed;
        failed.status = ExecutionStatus::kOverflow;
        return failed;
    }
    if (total_qty == 0) {
        Execution failed;
        failed.status = ExecutionStatus::kLimitExcludedAll;
        return failed;
    }

    result.qty = Qty{total_qty};
    result.vwap = Price{detail::conservative_vwap(notional, total_qty, side)};
    result.depth_exhausted = !stopped_at_limit;
    result.slippage = detail::bps_between(touch.price.ticks, result.vwap.ticks);
    result.status = ExecutionStatus::kOk;
    return result;
}

/// The quantity available at the touch alone.
///
/// Named separately because it is what most order book APIs return when asked
/// for "size", and conflating it with executable size is the mistake this
/// header exists to prevent.
template <typename SideImpl>
[[nodiscard]] Qty touch_size(const BasicL2Book<SideImpl>& book, Side side) {
    Level touch{};
    return book.best(side, touch) ? touch.qty : Qty{};
}

/// Cost of taking `side` for a given size, expressed as slippage against the
/// touch in basis points.
///
/// If the book cannot supply `wanted`, the result reports what it could supply
/// and flags exhaustion, rather than extrapolating a price for size that is not
/// there. Inventing depth is how a backtest produces returns a live account
/// cannot. Check `status` before the numbers: a non-positive `wanted` and an
/// overflowing book both come back empty and mean entirely different things.
template <typename SideImpl>
[[nodiscard]] Execution cost_to_trade(const BasicL2Book<SideImpl>& book, Side side, Qty wanted) {
    Execution result;
    if (wanted.units <= 0) {
        result.status = ExecutionStatus::kInvalidRequest;
        return result;
    }

    Level touch{};
    if (!book.best(side, touch)) {
        result.depth_exhausted = true;
        result.status = ExecutionStatus::kEmptyBook;
        return result;
    }

    std::int64_t notional = 0;
    std::int64_t remaining = wanted.units;
    bool overflowed = false;

    book.side(side).for_each([&](const Level& level) {
        const std::int64_t take = (level.qty.units < remaining) ? level.qty.units : remaining;

        std::int64_t contribution = 0;
        if (!detail::checked_mul_signed(level.price.ticks, take, contribution) ||
            !detail::checked_add_signed(notional, contribution, notional)) {
            overflowed = true;
            return false;
        }

        remaining -= take;
        result.limit_price = level.price;
        ++result.levels;
        return remaining > 0;
    });

    if (overflowed) {
        Execution failed;
        failed.status = ExecutionStatus::kOverflow;
        return failed;
    }

    const std::int64_t filled = wanted.units - remaining;
    if (filled == 0) {
        Execution failed;
        failed.depth_exhausted = true;
        failed.status = ExecutionStatus::kEmptyBook;
        return failed;
    }

    result.qty = Qty{filled};
    result.vwap = Price{detail::conservative_vwap(notional, filled, side)};
    result.depth_exhausted = (remaining > 0);
    result.slippage = detail::bps_between(touch.price.ticks, result.vwap.ticks);
    result.status = ExecutionStatus::kOk;
    return result;
}

}  // namespace crossbook
