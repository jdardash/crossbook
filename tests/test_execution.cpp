// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "crossbook/book.hpp"
#include "crossbook/execution.hpp"

using namespace crossbook;

namespace {

InstrumentSpec spec() { return InstrumentSpec{"BTC/USD", 1, 8}; }

/// Asks at 45283.6, .7, .8, ... each holding 1.0 (mantissa 100'000'000).
ArrayBook ladder(std::int64_t levels, std::int64_t qty_per_level = 100'000'000) {
    ArrayBook book(spec());
    for (std::int64_t i = 0; i < levels; ++i) {
        book.apply(Side::kAsk, Price{452836 + i}, Qty{qty_per_level});
        book.apply(Side::kBid, Price{452835 - i}, Qty{qty_per_level});
    }
    return book;
}

}  // namespace

TEST_CASE("an empty book has no executable size", "[execution]") {
    ArrayBook book(spec());
    const Execution e = executable_size(book, Side::kAsk, from_bps(1));
    CHECK(e.empty());
    CHECK(e.depth_exhausted);
    CHECK(e.levels == 0);
}

TEST_CASE("zero slippage returns only the touch", "[execution]") {
    // This is the honest version of the number most dashboards print as
    // "liquidity": what rests at the best price and nothing else.
    const ArrayBook book = ladder(10);
    const Execution e = executable_size(book, Side::kAsk, 0);
    CHECK(e.levels == 1);
    CHECK(e.qty == Qty{100'000'000});
    CHECK(e.vwap == Price{452836});
    CHECK(e.slippage == 0);
    CHECK(touch_size(book, Side::kAsk) == e.qty);
}

TEST_CASE("executable size grows with the slippage budget", "[execution]") {
    // The whole point: size is a function of how much you are willing to pay,
    // and a single "size" number without a price limit is meaningless.
    const ArrayBook book = ladder(200);

    const Execution tight = executable_size(book, Side::kAsk, from_bps(1));
    const Execution loose = executable_size(book, Side::kAsk, from_bps(20));

    CHECK(tight.qty.units < loose.qty.units);
    CHECK(tight.levels < loose.levels);
    CHECK(tight.slippage <= loose.slippage);
}

TEST_CASE("the walk stops at the price limit", "[execution]") {
    // Mind the units. 1 bps of 45283.6 is 4.53 price units, and a tick here is
    // 0.1 (price_scale 1), so the budget is ~45 TICKS — not 4.5. Confusing the
    // two is exactly the kind of scale error fixed-point exists to make
    // visible, and it caught this test rather than the code.
    const ArrayBook book = ladder(200);
    const Execution e = executable_size(book, Side::kAsk, from_bps(1));

    REQUIRE_FALSE(e.empty());
    CHECK(e.limit_price.ticks <= 452836 + 45);
    CHECK(e.limit_price.ticks > 452836 + 40);  // And it used most of the budget.
    CHECK_FALSE(e.depth_exhausted);  // Stopped by the limit, not by running out.
}

TEST_CASE("running out of depth is distinguished from hitting the limit", "[execution]") {
    // These mean opposite things operationally: one says widen the limit, the
    // other says find another venue. Conflating them would make the number
    // useless for routing.
    const ArrayBook shallow = ladder(3);

    const Execution exhausted = executable_size(shallow, Side::kAsk, from_percent(100));
    CHECK(exhausted.depth_exhausted);
    CHECK(exhausted.levels == 3);

    const ArrayBook deep = ladder(500);
    const Execution limited = executable_size(deep, Side::kAsk, from_bps(1));
    CHECK_FALSE(limited.depth_exhausted);
}

TEST_CASE("both sides walk away from the touch", "[execution]") {
    // Buying walks up the asks; selling walks down the bids. Getting the
    // direction wrong would report the book behind you as available.
    const ArrayBook book = ladder(50);

    const Execution buy = executable_size(book, Side::kAsk, from_bps(5));
    const Execution sell = executable_size(book, Side::kBid, from_bps(5));

    CHECK(buy.limit_price.ticks > 452836 - 1);   // Ascending from best ask.
    CHECK(sell.limit_price.ticks < 452835 + 1);  // Descending from best bid.
    CHECK(buy.vwap.ticks >= 452836);
    CHECK(sell.vwap.ticks <= 452835);
}

TEST_CASE("VWAP sits between the touch and the limit price", "[execution]") {
    const ArrayBook book = ladder(100);
    const Execution e = executable_size(book, Side::kAsk, from_bps(10));
    REQUIRE(e.levels > 1);
    CHECK(e.vwap.ticks >= 452836);
    CHECK(e.vwap.ticks <= e.limit_price.ticks);
}

TEST_CASE("VWAP is size-weighted, not a level average", "[execution]") {
    // A fat touch and thin levels behind it must pull the VWAP toward the
    // touch. An unweighted mean would overstate the cost.
    ArrayBook book(spec());
    book.apply(Side::kAsk, Price{452836}, Qty{1'000'000'000});  // 10 units
    book.apply(Side::kAsk, Price{452900}, Qty{100'000'000});    // 1 unit

    const Execution e = executable_size(book, Side::kAsk, from_percent(100));
    CHECK(e.levels == 2);
    // Weighted: (452836*10 + 452900*1) / 11 ~= 452841.8 -> truncated 452841.
    CHECK(e.vwap.ticks == 452841);
    // An unweighted mean would be (452836+452900)/2 = 452868.
    CHECK(e.vwap.ticks < 452868);
}

// ---------------------------------------------------------------------------
// cost_to_trade
// ---------------------------------------------------------------------------

TEST_CASE("cost_to_trade fills exactly the requested size", "[execution]") {
    const ArrayBook book = ladder(100);
    const Execution e = cost_to_trade(book, Side::kAsk, Qty{250'000'000});  // 2.5 units

    CHECK(e.qty == Qty{250'000'000});
    CHECK_FALSE(e.depth_exhausted);
    CHECK(e.levels == 3);  // Two full levels plus half of the third.
    CHECK(e.slippage >= 0);
}

TEST_CASE("cost_to_trade partially fills rather than inventing depth", "[execution]") {
    // Extrapolating a price for size that is not in the book is how a backtest
    // produces returns a live account cannot.
    const ArrayBook book = ladder(2);
    const Execution e = cost_to_trade(book, Side::kAsk, Qty{10'000'000'000});

    CHECK(e.depth_exhausted);
    CHECK(e.qty == Qty{200'000'000});  // Only what was actually there.
    CHECK(e.levels == 2);
}

TEST_CASE("cost_to_trade on an empty book reports nothing available", "[execution]") {
    ArrayBook book(spec());
    const Execution e = cost_to_trade(book, Side::kAsk, Qty{100});
    CHECK(e.empty());
    CHECK(e.depth_exhausted);
}

TEST_CASE("cost_to_trade rejects non-positive size", "[execution]") {
    const ArrayBook book = ladder(10);
    CHECK(cost_to_trade(book, Side::kAsk, Qty{0}).empty());
    CHECK(cost_to_trade(book, Side::kAsk, Qty{-5}).empty());
}

TEST_CASE("slippage rises with size", "[execution]") {
    // The number a carry trade's edge has to survive, and the reason sizing off
    // the touch overstates it.
    const ArrayBook book = ladder(500);
    const Execution small = cost_to_trade(book, Side::kAsk, Qty{100'000'000});
    const Execution large = cost_to_trade(book, Side::kAsk, Qty{20'000'000'000});

    CHECK(small.slippage <= large.slippage);
    CHECK(small.vwap.ticks <= large.vwap.ticks);
}

TEST_CASE("touch size overstates executable size on a thin book", "[execution]") {
    // The concrete failure this header exists to prevent. The touch shows 0.01
    // units; anyone sizing from it and trading 1 unit pays ~14 bps more than
    // the quote implied.
    ArrayBook book(spec());
    book.apply(Side::kAsk, Price{452836}, Qty{1'000'000});  // 0.01 units
    for (std::int64_t i = 1; i <= 100; ++i) {
        book.apply(Side::kAsk, Price{452836 + i * 6}, Qty{100'000'000});
    }

    const Qty at_touch = touch_size(book, Side::kAsk);
    const Execution real = cost_to_trade(book, Side::kAsk, Qty{100'000'000});

    CHECK(at_touch == Qty{1'000'000});
    CHECK(real.qty == Qty{100'000'000});
    CHECK(real.levels > 1);
    INFO("slippage=" << real.slippage << "bps over " << real.levels << " levels");
    CHECK(real.slippage > 0);
}

TEST_CASE("results are identical across both book implementations", "[execution]") {
    // Execution figures feed sizing decisions, so they must not depend on which
    // container produced the book.
    MapBook reference(spec());
    ArrayBook subject(spec());
    for (std::int64_t i = 0; i < 100; ++i) {
        reference.apply(Side::kAsk, Price{452836 + i}, Qty{100'000'000 + i});
        subject.apply(Side::kAsk, Price{452836 + i}, Qty{100'000'000 + i});
    }

    for (CentiBps budget : {CentiBps{0}, from_bps(1), from_bps(5), from_bps(50), from_bps(1000)}) {
        INFO("slippage budget " << budget << " centibps");
        const Execution a = executable_size(reference, Side::kAsk, budget);
        const Execution b = executable_size(subject, Side::kAsk, budget);
        CHECK(a.qty == b.qty);
        CHECK(a.vwap == b.vwap);
        CHECK(a.levels == b.levels);
        CHECK(a.depth_exhausted == b.depth_exhausted);
    }
}

TEST_CASE("a pathological book returns nothing rather than a wrapped number",
          "[execution]") {
    // A confidently wrong notional is worse than an empty answer.
    ArrayBook book(spec());
    book.apply(Side::kAsk, Price{4'000'000'000'000LL}, Qty{4'000'000'000'000LL});
    const Execution e = executable_size(book, Side::kAsk, from_percent(100));
    CHECK(e.empty());
}
