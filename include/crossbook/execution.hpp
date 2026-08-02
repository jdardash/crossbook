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

/// The result of walking a book to a price limit.
struct Execution {
    /// Total quantity available within the limit.
    Qty qty{};

    /// Volume-weighted average price, at the instrument's price scale.
    /// Zero when `qty` is zero.
    Price vwap{};

    /// The worst price touched. This, not the VWAP, is what a limit order needs.
    Price limit_price{};

    /// How many price levels were consumed.
    std::size_t levels{0};

    /// True if the walk ran out of book before reaching the price limit — the
    /// venue simply does not have more depth. Distinguishing this from "the
    /// limit stopped us" matters: one means widen the limit, the other means
    /// find another venue.
    bool depth_exhausted{false};

    /// Slippage of the VWAP against the touch, in hundredths of a basis point.
    /// The number a carry trade's edge has to survive.
    CentiBps slippage{0};

    [[nodiscard]] bool empty() const noexcept { return qty.units == 0; }
};

namespace detail {

/// The price limit `slippage_bps` away from `touch`, in the direction that
/// costs the taker money: buying walks up the asks, selling walks down the bids.
[[nodiscard]] constexpr std::int64_t price_limit(std::int64_t touch, Side side,
                                                 CentiBps slippage) noexcept {
    // Rounded toward the touch so the limit is never more permissive than asked.
    const std::int64_t offset = (touch * slippage) / 1'000'000;
    // Taking liquidity from the ask side means paying up; from the bid side,
    // selling down.
    return side == Side::kAsk ? touch + offset : touch - offset;
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
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    if (delta != 0 && delta > kMax / 1'000'000) {
        return 0;
    }
    return (delta * 1'000'000) / reference;
}

}  // namespace detail

/// Walk `side` of `book` from the touch, accumulating quantity until the price
/// moves more than `max_slippage` away from it.
///
/// `side` is the side being *taken*: kAsk to buy, kBid to sell.
///
/// Passing `max_slippage == 0` returns only what rests at the touch, which
/// is the honest version of the number most dashboards print as "liquidity".
template <typename SideImpl>
[[nodiscard]] Execution executable_size(const BasicL2Book<SideImpl>& book, Side side,
                                        CentiBps max_slippage) {
    Execution result;

    Level touch{};
    if (!book.best(side, touch)) {
        result.depth_exhausted = true;
        return result;
    }

    const std::int64_t limit = detail::price_limit(touch.price.ticks, side, max_slippage);

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

        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        if (level.qty.units != 0 && level.price.ticks > kMax / level.qty.units) {
            overflowed = true;
            return false;
        }
        const std::int64_t contribution = level.price.ticks * level.qty.units;
        if (notional > kMax - contribution || total_qty > kMax - level.qty.units) {
            overflowed = true;
            return false;
        }

        notional += contribution;
        total_qty += level.qty.units;
        result.limit_price = level.price;
        ++result.levels;
        return true;
    });

    if (overflowed || total_qty == 0) {
        // An overflowing book is not a book anyone should trade against, and
        // reporting a wrapped number would be worse than reporting nothing.
        return Execution{};
    }

    result.qty = Qty{total_qty};
    result.vwap = Price{notional / total_qty};  // Truncating; documented.
    result.depth_exhausted = !stopped_at_limit;
    result.slippage = detail::bps_between(touch.price.ticks, result.vwap.ticks);
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
/// Returns `std::nullopt`-equivalent behaviour via `depth_exhausted`: if the
/// book cannot supply `wanted`, the result reports what it could supply and
/// flags exhaustion, rather than extrapolating a price for size that is not
/// there. Inventing depth is how a backtest produces returns a live account
/// cannot.
template <typename SideImpl>
[[nodiscard]] Execution cost_to_trade(const BasicL2Book<SideImpl>& book, Side side, Qty wanted) {
    Execution result;
    if (wanted.units <= 0) {
        return result;
    }

    Level touch{};
    if (!book.best(side, touch)) {
        result.depth_exhausted = true;
        return result;
    }

    std::int64_t notional = 0;
    std::int64_t remaining = wanted.units;
    bool overflowed = false;

    book.side(side).for_each([&](const Level& level) {
        const std::int64_t take = (level.qty.units < remaining) ? level.qty.units : remaining;

        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        if (take != 0 && level.price.ticks > kMax / take) {
            overflowed = true;
            return false;
        }
        const std::int64_t contribution = level.price.ticks * take;
        if (notional > kMax - contribution) {
            overflowed = true;
            return false;
        }

        notional += contribution;
        remaining -= take;
        result.limit_price = level.price;
        ++result.levels;
        return remaining > 0;
    });

    if (overflowed) {
        return Execution{};
    }

    const std::int64_t filled = wanted.units - remaining;
    if (filled == 0) {
        result.depth_exhausted = true;
        return result;
    }

    result.qty = Qty{filled};
    result.vwap = Price{notional / filled};
    result.depth_exhausted = (remaining > 0);
    result.slippage = detail::bps_between(touch.price.ticks, result.vwap.ticks);
    return result;
}

}  // namespace crossbook
