// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>

#include "crossbook/book.hpp"
#include "crossbook/consolidated.hpp"

using namespace crossbook;

namespace {

InstrumentSpec spec() { return InstrumentSpec{"BTC/USD", 1, 8}; }

VenueQuote quote(std::string venue, std::int64_t bid, std::int64_t ask, std::int64_t taker_bps,
                 Timestamp received_at = 1'000'000'000, std::int64_t qty = 100'000'000) {
    VenueQuote q;
    q.venue = std::move(venue);
    q.bid = Level{Price{bid}, Qty{qty}};
    q.ask = Level{Price{ask}, Qty{qty}};
    q.has_bid = true;
    q.has_ask = true;
    q.received_at = received_at;
    q.fees.taker = from_bps(taker_bps);
    return q;
}

}  // namespace

TEST_CASE("an empty consolidated book has no best price", "[consolidated]") {
    ConsolidatedBook book(spec());
    EffectivePrice best{};
    CHECK_FALSE(book.best(Side::kAsk, 1'000'000'000, best));
    CHECK(book.venue_count() == 0);
}

TEST_CASE("updating a venue replaces rather than duplicates", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("kraken", 452835, 452836, 10));
    book.update(quote("kraken", 452840, 452841, 10));
    CHECK(book.venue_count() == 1);

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 1'000'000'000, best));
    CHECK(best.quoted == Price{452841});
}

TEST_CASE("remove drops a venue", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("kraken", 452835, 452836, 10));
    book.update(quote("binance", 452834, 452837, 10));
    book.remove("kraken");
    CHECK(book.venue_count() == 1);

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 1'000'000'000, best));
    CHECK(best.venue == "binance");
}

// ---------------------------------------------------------------------------
// The finding worth publishing: fees invert the ranking
// ---------------------------------------------------------------------------

TEST_CASE("fees change which venue is actually cheapest", "[consolidated][fees]") {
    // THE POINT OF FEE ADJUSTMENT.
    //
    // Kraken shows the better headline ask — 45283.6 versus 45283.9 — so any
    // dashboard comparing raw prices calls Kraken best. But Kraken charges 26
    // bps and the other venue charges 1 bp, and on a ~45283 price that fee
    // difference is worth ~113 ticks while the quote difference is worth 3.
    //
    // Comparing raw prices across venues is simply wrong, and this is the case
    // that shows it.
    ConsolidatedBook book(spec());
    book.update(quote("kraken", 452830, 452836, 26));
    book.update(quote("cheapfees", 452829, 452839, 1));

    const auto raw_winner = 452836;  // What a naive comparison would pick.

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 1'000'000'000, best));

    CHECK(best.venue == "cheapfees");
    CHECK(best.quoted.ticks > raw_winner);       // Worse headline price...
    CHECK(best.effective.ticks < 452836 + 117);  // ...but cheaper to actually trade.
}

TEST_CASE("fee direction differs by side", "[consolidated][fees]") {
    // Lifting an ask costs the fee on top; hitting a bid nets it out. Applying
    // the same sign to both would make one side systematically wrong.
    ConsolidatedBook book(spec());
    book.update(quote("v", 452830, 452836, 100));  // 1% taker

    EffectivePrice ask{};
    EffectivePrice bid{};
    REQUIRE(book.best(Side::kAsk, 1'000'000'000, ask));
    REQUIRE(book.best(Side::kBid, 1'000'000'000, bid));

    CHECK(ask.effective.ticks > ask.quoted.ticks);  // Buying costs more.
    CHECK(bid.effective.ticks < bid.quoted.ticks);  // Selling receives less.
}

TEST_CASE("ranking orders by effective price on both sides", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));
    book.update(quote("b", 452835, 452838, 0));
    book.update(quote("c", 452833, 452836, 0));

    const auto asks = book.ranked(Side::kAsk, 1'000'000'000);
    REQUIRE(asks.size() == 3);
    CHECK(asks[0].venue == "c");  // Lowest ask wins when buying.
    CHECK(asks[2].venue == "a");

    const auto bids = book.ranked(Side::kBid, 1'000'000'000);
    REQUIRE(bids.size() == 3);
    CHECK(bids[0].venue == "b");  // Highest bid wins when selling.
    CHECK(bids[2].venue == "a");
}

// ---------------------------------------------------------------------------
// Staleness and sync
// ---------------------------------------------------------------------------

TEST_CASE("a stale venue is excluded entirely", "[consolidated][staleness]") {
    // A venue that has gone quiet still has a book in memory and looks exactly
    // like a stable market. Quoting it as best is worse than dropping it.
    ConsolidatedBook book(spec(), 5'000'000'000);  // 5s
    book.update(quote("fresh", 452830, 452840, 0, 10'000'000'000));
    book.update(quote("stale", 452835, 452836, 0, 1'000'000'000));  // 9s old

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 10'000'000'000, best));
    CHECK(best.venue == "fresh");  // Despite the stale venue quoting better.
    CHECK(book.ranked(Side::kAsk, 10'000'000'000).size() == 1);
}

TEST_CASE("an unsynced venue is excluded", "[consolidated]") {
    // A book known to be wrong must not contribute to a best-price calculation,
    // however attractive its levels look.
    ConsolidatedBook book(spec());
    VenueQuote broken = quote("broken", 452835, 452836, 0);
    broken.synced = false;
    book.update(broken);
    book.update(quote("healthy", 452830, 452840, 0));

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 1'000'000'000, best));
    CHECK(best.venue == "healthy");
}

TEST_CASE("a venue quoting only one side counts only on that side", "[consolidated]") {
    ConsolidatedBook book(spec());
    VenueQuote bid_only = quote("bidonly", 452900, 0, 0);
    bid_only.has_ask = false;
    book.update(bid_only);
    book.update(quote("both", 452830, 452840, 0));

    CHECK(book.ranked(Side::kBid, 1'000'000'000).size() == 2);
    CHECK(book.ranked(Side::kAsk, 1'000'000'000).size() == 1);
}

TEST_CASE("zero max_age disables staleness filtering", "[consolidated][staleness]") {
    ConsolidatedBook book(spec(), 0);
    book.update(quote("ancient", 452835, 452836, 0, 1));
    CHECK(book.ranked(Side::kAsk, 999'999'999'999).size() == 1);
}

// ---------------------------------------------------------------------------
// Spread and crossing
// ---------------------------------------------------------------------------

TEST_CASE("consolidated spread is fee-inclusive", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));

    CentiBps zero_fee = 0;
    REQUIRE(book.spread(1'000'000'000, zero_fee));

    ConsolidatedBook with_fees(spec());
    with_fees.update(quote("a", 452830, 452840, 50));
    CentiBps fee_laden = 0;
    REQUIRE(with_fees.spread(1'000'000'000, fee_laden));

    CHECK(fee_laden > zero_fee);  // Fees widen the real spread.
}

TEST_CASE("a genuinely crossed market is reported, not clamped", "[consolidated]") {
    // Two venues really can cross. Hiding it behind a clamp would erase the
    // only interesting reading this number produces — while noting that a
    // crossed quote is not the same thing as an executable arbitrage.
    ConsolidatedBook book(spec());
    book.update(quote("rich_bid", 452900, 453000, 0));
    book.update(quote("cheap_ask", 452000, 452100, 0));

    CentiBps spread_value = 0;
    REQUIRE(book.spread(1'000'000'000, spread_value));
    CHECK(spread_value < 0);
    CHECK(book.crossed(1'000'000'000));
}

TEST_CASE("an uncrossed market reports a positive spread", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));
    CentiBps spread_value = 0;
    REQUIRE(book.spread(1'000'000'000, spread_value));
    CHECK(spread_value > 0);
    CHECK_FALSE(book.crossed(1'000'000'000));
}

TEST_CASE("spread is unavailable when a side has no usable venue", "[consolidated]") {
    ConsolidatedBook book(spec());
    VenueQuote bid_only = quote("bidonly", 452900, 0, 0);
    bid_only.has_ask = false;
    book.update(bid_only);

    CentiBps spread_value = 0;
    CHECK_FALSE(book.spread(1'000'000'000, spread_value));
    CHECK_FALSE(book.crossed(1'000'000'000));
}

// ---------------------------------------------------------------------------
// best_execution — the function the library builds toward
// ---------------------------------------------------------------------------

namespace {

ArrayBook ladder(std::int64_t best_ask, std::int64_t levels, std::int64_t qty_per_level) {
    ArrayBook book(spec());
    for (std::int64_t i = 0; i < levels; ++i) {
        book.apply(Side::kAsk, Price{best_ask + i}, Qty{qty_per_level});
        book.apply(Side::kBid, Price{best_ask - 1 - i}, Qty{qty_per_level});
    }
    return book;
}

}  // namespace

TEST_CASE("best_execution walks real depth rather than comparing touches",
          "[consolidated][execution]") {
    // THE SIZE INVERSION.
    //
    // "thin" shows the better touch and wins any quote comparison. It has 0.01
    // units there and nothing behind it for 50 ticks. "deep" is a tick worse at
    // the touch and has real size.
    //
    // At one coin, "deep" is cheaper. A size-free "best venue" is not a
    // well-defined question, which is why this takes a quantity.
    std::map<std::string, ArrayBook> books;
    books.emplace("thin", ladder(452836, 1, 1'000'000));
    books.emplace("deep", ladder(452837, 200, 100'000'000));
    // Give "thin" a far-away second level so it can fill, expensively.
    books.at("thin").apply(Side::kAsk, Price{452890}, Qty{10'000'000'000});

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("thin", 452835, 452836, 0));
    consolidated.update(quote("deep", 452835, 452837, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    // Best touch is "thin".
    EffectivePrice touch_best{};
    REQUIRE(consolidated.best(Side::kAsk, 1'000'000'000, touch_best));
    CHECK(touch_best.venue == "thin");

    // Best execution for a whole coin is not.
    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, 1'000'000'000, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "deep");
    CHECK(best.venues_considered == 2);
    CHECK_FALSE(best.execution.depth_exhausted);
}

TEST_CASE("best_execution prefers a complete fill over a better partial",
          "[consolidated][execution]") {
    // A partial fill at a good price is not a fill: the remainder has to trade
    // somewhere, and pricing it as free is how a router lies about its cost.
    std::map<std::string, ArrayBook> books;
    books.emplace("cheap_shallow", ladder(452800, 1, 1'000'000));
    books.emplace("pricier_deep", ladder(452900, 100, 100'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("cheap_shallow", 452799, 452800, 0));
    consolidated.update(quote("pricier_deep", 452899, 452900, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{500'000'000}, 1'000'000'000, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "pricier_deep");
    CHECK_FALSE(best.execution.depth_exhausted);
}

TEST_CASE("best_execution applies fees to the achieved VWAP", "[consolidated][execution]") {
    std::map<std::string, ArrayBook> books;
    books.emplace("free", ladder(452840, 100, 100'000'000));
    books.emplace("expensive", ladder(452836, 100, 100'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("free", 452839, 452840, 0));
    consolidated.update(quote("expensive", 452835, 452836, 50));  // 50 bps

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, 1'000'000'000, lookup);
    REQUIRE(best.found);
    // 50 bps on ~452836 is ~226 ticks, dwarfing the 4-tick quote advantage.
    CHECK(best.venue == "free");
    CHECK(best.effective_vwap.ticks == best.execution.vwap.ticks);  // Zero fee venue.
}

TEST_CASE("best_execution skips venues with no book", "[consolidated][execution]") {
    std::map<std::string, ArrayBook> books;
    books.emplace("present", ladder(452840, 50, 100'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("present", 452839, 452840, 0));
    consolidated.update(quote("absent", 452835, 452836, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, 1'000'000'000, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "present");
    CHECK(best.venues_considered == 1);
}

TEST_CASE("best_execution finds nothing when every venue is stale",
          "[consolidated][execution]") {
    std::map<std::string, ArrayBook> books;
    books.emplace("old", ladder(452840, 50, 100'000'000));

    ConsolidatedBook consolidated(spec(), 1'000'000'000);
    consolidated.update(quote("old", 452839, 452840, 0, 1));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, 999'999'999'999, lookup);
    CHECK_FALSE(best.found);
    CHECK(best.venues_considered == 0);
}
