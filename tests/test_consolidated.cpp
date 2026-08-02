// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include "crossbook/book.hpp"
#include "crossbook/consolidated.hpp"

// The allocation probe lives in test_no_alloc.cpp, which replaces the global
// operator new for the whole test binary. That file is not ours to edit, so the
// counters are reached by declaration rather than by adding a case there. The
// alternative — asserting "best() is allocation-free" in prose — is exactly the
// decorative control this audit was about.
namespace alloc_probe {
extern std::atomic<bool> armed;
extern std::atomic<std::uint64_t> allocations;
}  // namespace alloc_probe

using namespace crossbook;

namespace {

constexpr Timestamp kNow = 1'000'000'000;

InstrumentSpec spec() { return InstrumentSpec{"BTC/USD", 1, 8}; }

VenueQuote quote(std::string venue, std::int64_t bid, std::int64_t ask, std::int64_t taker_bps,
                 Timestamp received_at = kNow, std::int64_t qty = 100'000'000) {
    VenueQuote q;
    q.venue = std::move(venue);
    q.spec = spec();
    q.bid = Level{Price{bid}, Qty{qty}};
    q.ask = Level{Price{ask}, Qty{qty}};
    q.has_bid = true;
    q.has_ask = true;
    q.received_at = received_at;
    q.fees.taker = from_bps(taker_bps);
    return q;
}

/// Arm the probe for a scope. Nothing inside a probed region may assert: a
/// Catch2 failure unwinding past the disarm would leave the counter live and
/// fail every later test for the wrong reason.
struct AllocGuard {
    AllocGuard() noexcept {
        alloc_probe::allocations.store(0, std::memory_order_relaxed);
        alloc_probe::armed.store(true, std::memory_order_relaxed);
    }
    ~AllocGuard() { alloc_probe::armed.store(false, std::memory_order_relaxed); }
    [[nodiscard]] static std::uint64_t count() noexcept {
        return alloc_probe::allocations.load(std::memory_order_relaxed);
    }
};

}  // namespace

TEST_CASE("an empty consolidated book has no best price", "[consolidated]") {
    ConsolidatedBook book(spec());
    EffectivePrice best{};
    CHECK_FALSE(book.best(Side::kAsk, kNow, best));
    CHECK(book.venue_count() == 0);
}

TEST_CASE("updating a venue replaces rather than duplicates", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("kraken", 452835, 452836, 10));
    book.update(quote("kraken", 452840, 452841, 10));
    CHECK(book.venue_count() == 1);

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, kNow, best));
    CHECK(best.quoted == Price{452841});
}

TEST_CASE("remove drops a venue", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("kraken", 452835, 452836, 10));
    book.update(quote("binance", 452834, 452837, 10));
    book.remove("kraken");
    CHECK(book.venue_count() == 1);

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, kNow, best));
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
    REQUIRE(book.best(Side::kAsk, kNow, best));

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
    REQUIRE(book.best(Side::kAsk, kNow, ask));
    REQUIRE(book.best(Side::kBid, kNow, bid));

    CHECK(ask.effective.ticks > ask.quoted.ticks);  // Buying costs more.
    CHECK(bid.effective.ticks < bid.quoted.ticks);  // Selling receives less.
}

TEST_CASE("the fee adjustment rounds away from zero on both sides",
          "[consolidated][fees][rounding]") {
    // THE DEFECT: `(price * taker) / 1'000'000` truncates toward zero, so the
    // adjustment was rounded DOWN on both sides — understating the fee on an
    // ask (the venue looked cheaper to buy on than it is) and overstating the
    // proceeds on a bid (it looked like it pays more than it does). That is the
    // same direction twice: both flatter the venue, and both push the
    // consolidated market toward looking crossed when it is not.
    //
    // 452836 * 2600 / 1'000'000 = 1177.3736 exactly. Truncated: 1177. Rounded
    // away from zero, which is the only direction a cost may round: 1178.
    ConsolidatedBook book(spec());
    book.update(quote("v", 452836, 452836, 26));

    EffectivePrice ask{};
    EffectivePrice bid{};
    REQUIRE(book.best(Side::kAsk, kNow, ask));
    REQUIRE(book.best(Side::kBid, kNow, bid));

    CHECK(ask.effective == Price{452836 + 1178});
    CHECK(bid.effective == Price{452836 - 1178});
}

TEST_CASE("ranking orders by effective price on both sides", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));
    book.update(quote("b", 452835, 452838, 0));
    book.update(quote("c", 452833, 452836, 0));

    const auto asks = book.ranked(Side::kAsk, kNow);
    REQUIRE(asks.size() == 3);
    CHECK(asks[0].venue == "c");  // Lowest ask wins when buying.
    CHECK(asks[2].venue == "a");

    const auto bids = book.ranked(Side::kBid, kNow);
    REQUIRE(bids.size() == 3);
    CHECK(bids[0].venue == "b");  // Highest bid wins when selling.
    CHECK(bids[2].venue == "a");
}

// ---------------------------------------------------------------------------
// Scale and symbol — a mantissa is not a price
// ---------------------------------------------------------------------------

TEST_CASE("a quote at a different price scale is refused", "[consolidated][scale]") {
    // THE DEFECT, AND THE ONE THAT LOST MONEY SILENTLY. The consolidated book
    // stored an InstrumentSpec, exposed it, and never read it anywhere. Kraken
    // quotes BTC/USD at price_scale 1, so 45283.6 arrives as 452836; Binance
    // quotes the same instrument at price_scale 2, so 45283.50 arrives as
    // 4528350. Fed into one book unchecked, the higher-scaled venue wins every
    // bid ranking and loses every ask ranking forever, and `crossed()` fires
    // permanently on a market that is not crossed by a single tick.
    ConsolidatedBook book(spec());
    REQUIRE(book.update(quote("kraken", 452835, 452836, 0)));

    VenueQuote binance = quote("binance", 4'528'350, 4'528'360, 0);
    binance.spec = InstrumentSpec{"BTC/USD", 2, 8};  // Same instrument, other scale.
    CHECK_FALSE(book.update(binance));

    CHECK(book.venue_count() == 1);
    CHECK(book.rejected_updates() == 1);

    EffectivePrice best_bid{};
    REQUIRE(book.best(Side::kBid, kNow, best_bid));
    CHECK(best_bid.venue == "kraken");
    CHECK_FALSE(book.crossed(kNow));  // Not a 10x arbitrage. Not an arbitrage at all.
}

TEST_CASE("a quote for a different symbol is refused", "[consolidated][scale]") {
    // Same hole, different cause: nothing checked InstrumentSpec::symbol, so a
    // feed-handler routing bug dropping an ETH quote into a BTC book produced a
    // confident and entirely fictional arbitrage. Binance's combined-stream
    // envelope makes exactly this mis-routing easy to write.
    ConsolidatedBook book(spec());
    REQUIRE(book.update(quote("kraken", 452835, 452836, 0)));

    VenueQuote eth = quote("kraken-eth", 300'000, 300'010, 0);
    eth.spec = InstrumentSpec{"ETH/USD", 1, 8};
    CHECK_FALSE(book.update(eth));

    CHECK(book.venue_count() == 1);
    CHECK(book.rejected_updates() == 1);
    CHECK_FALSE(book.crossed(kNow));
}

TEST_CASE("a quantity scale mismatch is refused before it can corrupt a weight",
          "[consolidated][scale]") {
    // The invisible one. A price-scale mix at least shows up as an absurd
    // ranking; a quantity-scale mix produces prices that all look reasonable
    // and VWAP *weights* that are wrong by a factor of ten, so the reported
    // cost of a trade is wrong with no symptom at all.
    ConsolidatedBook book(spec());
    VenueQuote wrong_qty = quote("venue", 452835, 452836, 0);
    wrong_qty.spec = InstrumentSpec{"BTC/USD", 1, 7};
    CHECK_FALSE(book.update(wrong_qty));
    CHECK(book.venue_count() == 0);
    CHECK(book.rejected_updates() == 1);
}

TEST_CASE("a negative taker fee is refused", "[consolidated][fees]") {
    // A maker rebate is a different question, as FeeSchedule says. Applied here
    // as a negative taker it makes an ask look cheaper than the venue is
    // actually quoting it — the one direction that costs money.
    ConsolidatedBook book(spec());
    VenueQuote rebate = quote("rebate", 452835, 452836, 0);
    rebate.fees.taker = from_bps(-5);
    CHECK_FALSE(book.update(rebate));
    CHECK(book.venue_count() == 0);
    CHECK(book.rejected_updates() == 1);
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

TEST_CASE("an unstamped quote is unusable, not immortal", "[consolidated][staleness]") {
    // THE DEFECT: the guard read `received_at > 0` as a precondition for
    // EXCLUDING a quote, so the struct default of zero — much the likeliest
    // mistake in caller code — produced a venue that was permanently fresh and
    // could be reported as best forever, including one that disconnected at
    // session start. The default has to fail closed.
    ConsolidatedBook book(spec(), 5'000'000'000);
    VenueQuote unstamped = quote("forgot-to-stamp", 452900, 452800, 0);
    unstamped.received_at = 0;
    book.update(unstamped);
    book.update(quote("stamped", 452830, 452840, 0, 10'000'000'000));

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, 10'000'000'000, best));
    CHECK(best.venue == "stamped");  // Despite the unstamped venue quoting better.
    CHECK(book.ranked(Side::kAsk, 10'000'000'000).size() == 1);
    CHECK(book.ranked(Side::kBid, 10'000'000'000).size() == 1);
}

TEST_CASE("a quote timestamped in the future is a clock fault, not eternal freshness",
          "[consolidated][staleness]") {
    // THE DEFECT: `now > q.received_at` was one of four conditions required to
    // exclude, so a quote stamped ahead of local time was never stale. Point a
    // venue clock — or a feed handler that stamped with the venue's timestamp
    // instead of local receive time, which this header explicitly warns against
    // and nothing enforced — a few seconds fast, and that venue is exempt from
    // staleness filtering permanently.
    ConsolidatedBook book(spec(), 5'000'000'000);
    book.update(quote("fast-clock", 452900, 452800, 0, kNow + 60'000'000'000));
    book.update(quote("honest", 452830, 452840, 0, kNow));

    EffectivePrice best{};
    REQUIRE(book.best(Side::kAsk, kNow, best));
    CHECK(best.venue == "honest");
    CHECK(book.ranked(Side::kAsk, kNow).size() == 1);
}

TEST_CASE("a negative max age does not quietly disable the filter",
          "[consolidated][staleness]") {
    // THE DEFECT: `max_age_ns_ > 0` gated the whole guard, so a negative max age
    // disabled staleness filtering entirely — on a configuration that reads
    // like an unusually strict one. Only the zero case was documented and
    // tested. An age limit that cannot be satisfied must exclude everything,
    // not everything's opposite.
    ConsolidatedBook book(spec(), -5'000'000'000);
    book.update(quote("ancient", 452835, 452836, 0, 1));
    CHECK(book.ranked(Side::kAsk, 999'999'999'999).empty());
    CHECK_FALSE(book.crossed(999'999'999'999));

    // Zero is the same argument: "exclude anything older than nothing".
    ConsolidatedBook zero(spec(), 0);
    zero.update(quote("ancient", 452835, 452836, 0, 1));
    CHECK(zero.ranked(Side::kAsk, 999'999'999'999).empty());
}

TEST_CASE("the staleness boundary is exact", "[consolidated][staleness]") {
    // Untested before: the only staleness case ran 9 seconds against a 5 second
    // limit, so every off-by-one in the comparison passed. The documented rule
    // is that entries OLDER THAN the limit are excluded, which puts an age of
    // exactly max_age on the usable side — deliberately, rather than by
    // whichever comparison operator happened to be typed.
    constexpr Timestamp kMaxAge = 5'000'000'000;
    ConsolidatedBook book(spec(), kMaxAge);

    book.update(quote("v", 452835, 452836, 0, kNow));
    CHECK(book.ranked(Side::kAsk, kNow + kMaxAge).size() == 1);      // Exactly at the limit.
    CHECK(book.ranked(Side::kAsk, kNow + kMaxAge + 1).empty());      // One ns past it.
    CHECK(book.ranked(Side::kAsk, kNow + kMaxAge - 1).size() == 1);  // One ns short.
}

TEST_CASE("age filtering is switched off by policy, not by a magic zero",
          "[consolidated][staleness]") {
    // Replay of a historical capture is a real use for this, and it needs its
    // own name rather than an overloaded duration. Under kDisabled an unstamped
    // quote is usable again, which is precisely why the opt-out is explicit.
    ConsolidatedBook book(spec(), ConsolidatedBook::kDefaultMaxAgeNs,
                          StalenessPolicy::kDisabled);
    book.update(quote("ancient", 452835, 452836, 0, 1));
    VenueQuote unstamped = quote("unstamped", 452830, 452840, 0);
    unstamped.received_at = 0;
    book.update(unstamped);

    CHECK(book.ranked(Side::kAsk, 999'999'999'999).size() == 2);
    CHECK(book.staleness_policy() == StalenessPolicy::kDisabled);
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
    REQUIRE(book.best(Side::kAsk, kNow, best));
    CHECK(best.venue == "healthy");
}

TEST_CASE("a venue quoting only one side counts only on that side", "[consolidated]") {
    ConsolidatedBook book(spec());
    VenueQuote bid_only = quote("bidonly", 452900, 0, 0);
    bid_only.has_ask = false;
    book.update(bid_only);
    book.update(quote("both", 452830, 452840, 0));

    CHECK(book.ranked(Side::kBid, kNow).size() == 2);
    CHECK(book.ranked(Side::kAsk, kNow).size() == 1);
}

// ---------------------------------------------------------------------------
// Determinism: ties must not be broken by arrival order
// ---------------------------------------------------------------------------

TEST_CASE("ties are broken by venue name, not by which venue connected first",
          "[consolidated][determinism]") {
    // THE DEFECT: both the ranking sort and the best-price scan compared
    // strictly, so equal effective prices were left in `quotes_` order — which
    // is first-update order, which is per-process state. Two processes reading
    // the same market would route differently, in a library whose headline
    // claim is determinism. And the truncation fixed elsewhere in this pass was
    // actively MANUFACTURING these ties by collapsing distinct effective prices
    // onto the same integer, so this is not a rare path.
    ConsolidatedBook first(spec());
    first.update(quote("zulu", 452835, 452836, 0));
    first.update(quote("alpha", 452835, 452836, 0));

    ConsolidatedBook second(spec());
    second.update(quote("alpha", 452835, 452836, 0));
    second.update(quote("zulu", 452835, 452836, 0));

    for (const Side side : {Side::kAsk, Side::kBid}) {
        EffectivePrice a{};
        EffectivePrice b{};
        REQUIRE(first.best(side, kNow, a));
        REQUIRE(second.best(side, kNow, b));
        CHECK(a.venue == "alpha");
        CHECK(b.venue == "alpha");
        CHECK(first.ranked(side, kNow).front().venue == "alpha");
        CHECK(second.ranked(side, kNow).front().venue == "alpha");
    }
}

// ---------------------------------------------------------------------------
// Spread and crossing
// ---------------------------------------------------------------------------

TEST_CASE("consolidated spread is fee-inclusive", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));

    CentiBps zero_fee = 0;
    REQUIRE(book.spread(kNow, zero_fee));

    ConsolidatedBook with_fees(spec());
    with_fees.update(quote("a", 452830, 452840, 50));
    CentiBps fee_laden = 0;
    REQUIRE(with_fees.spread(kNow, fee_laden));

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
    REQUIRE(book.spread(kNow, spread_value));
    CHECK(spread_value < 0);
    CHECK(book.crossed(kNow));
}

TEST_CASE("an uncrossed market reports a positive spread", "[consolidated]") {
    ConsolidatedBook book(spec());
    book.update(quote("a", 452830, 452840, 0));
    CentiBps spread_value = 0;
    REQUIRE(book.spread(kNow, spread_value));
    CHECK(spread_value > 0);
    CHECK_FALSE(book.crossed(kNow));
}

TEST_CASE("spread is unavailable when a side has no usable venue", "[consolidated]") {
    ConsolidatedBook book(spec());
    VenueQuote bid_only = quote("bidonly", 452900, 0, 0);
    bid_only.has_ask = false;
    book.update(bid_only);

    CentiBps spread_value = 0;
    CHECK_FALSE(book.spread(kNow, spread_value));
    CHECK_FALSE(book.crossed(kNow));
}

TEST_CASE("the mid does not overflow and a non-positive mid is refused",
          "[consolidated][overflow]") {
    // THE DEFECT: `mid = (bid + ask) / 2`, the textbook overflow, on two
    // mantissas the caller supplies. Anchoring on the bid and adding half the
    // delta keeps every intermediate between the operands. (Before the fix this
    // case was signed overflow — undefined behaviour that in practice wrapped
    // the mid negative and inverted the sign of the answer.)
    constexpr std::int64_t kMax = (std::numeric_limits<std::int64_t>::max)();
    ConsolidatedBook huge(spec());
    huge.update(quote("v", kMax - 10, kMax - 5, 0));

    CentiBps spread_value = -1;
    REQUIRE(huge.spread(kNow, spread_value));
    CHECK(spread_value >= 0);       // A five-tick spread on an enormous price.
    CHECK_FALSE(huge.crossed(kNow));

    // And a negative mid is refused rather than silently flipping the sign: an
    // uncrossed market would otherwise report a negative spread.
    ConsolidatedBook negative(spec());
    negative.update(quote("v", -1000, -900, 0));
    CentiBps unused = 0;
    CHECK_FALSE(negative.spread(kNow, unused));
}

// ---------------------------------------------------------------------------
// The touch-read path allocates nothing
// ---------------------------------------------------------------------------

TEST_CASE("reading the consolidated touch does not allocate", "[consolidated][alloc]") {
    // THE DEFECT: `best()` called `ranked()`, which heap-allocated a vector of
    // EffectivePrice, each holding a std::string venue BY VALUE — one
    // allocation per name past SSO, and "coinbase-exchange" is 17 characters —
    // and then insertion-sorted ALL venues, copying those strings on every
    // step, merely to read `front()`. `spread()` calls `best()` twice and
    // `crossed()` calls `spread()`, so "what is the best price right now"
    // allocated and sorted three deep, one layer above the book that documents
    // at length how it made that same question O(1) and allocation-free.
    ConsolidatedBook book(spec());
    book.update(quote("coinbase-exchange", 452830, 452840, 10));  // Past SSO.
    book.update(quote("kraken", 452835, 452838, 26));
    book.update(quote("binance", 452833, 452836, 1));

    EffectivePrice warm{};
    REQUIRE(book.best(Side::kAsk, kNow, warm));

    std::uint64_t observed = 0;
    std::int64_t sink = 0;
    {
        const AllocGuard guard;
        for (int i = 0; i < 1000; ++i) {
            EffectivePrice p{};
            if (book.best(Side::kAsk, kNow, p)) {
                sink += p.effective.ticks;
            }
            CentiBps s = 0;
            if (book.spread(kNow, s)) {
                sink += s;
            }
            std::int64_t ticks = 0;
            if (book.spread_ticks(kNow, ticks)) {
                sink += ticks;
            }
            sink += book.crossed(kNow) ? 1 : 0;
        }
        observed = AllocGuard::count();
    }
    CHECK(observed == 0);
    CHECK(sink != 0);
}

// ---------------------------------------------------------------------------
// best_execution — the function the library builds toward
// ---------------------------------------------------------------------------

namespace {

ArrayBook ladder(std::int64_t best_ask, std::int64_t levels, std::int64_t qty_per_level,
                 InstrumentSpec instrument = spec(), Timestamp stamped_at = kNow) {
    ArrayBook book(std::move(instrument));
    for (std::int64_t i = 0; i < levels; ++i) {
        book.apply(Side::kAsk, Price{best_ask + i}, Qty{qty_per_level});
        book.apply(Side::kBid, Price{best_ask - 1 - i}, Qty{qty_per_level});
    }
    book.set_last_update(stamped_at);
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
    REQUIRE(consolidated.best(Side::kAsk, kNow, touch_best));
    CHECK(touch_best.venue == "thin");

    // Best execution for a whole coin is not.
    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "deep");
    CHECK(best.venues_considered == 2);
    CHECK(best.venues_rejected == 0);
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
        best_execution(consolidated, Side::kAsk, Qty{500'000'000}, kNow, lookup);
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
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    // 50 bps on ~452836 is ~226 ticks, dwarfing the 4-tick quote advantage.
    CHECK(best.venue == "free");
    CHECK(best.effective_vwap.ticks == best.execution.vwap.ticks);  // Zero fee venue.
}

TEST_CASE("optimistic rounding inverts the cheapest venue", "[consolidated][execution][rounding]") {
    // THE WORKED EXAMPLE. Everything here truncated toward zero, and truncation
    // on a cost is always in the trader's favour, so the error accumulated in
    // one direction and was large enough to invert the answer:
    //
    //   alpha  453073 @ 0.01 then 453074 @ 0.99, taker 26 bps
    //          truncated VWAP 453073, truncated fee 1177 -> 454250 reported
    //          TRUE cost 453073.99 * 1.0026 = 454251.98
    //   bravo  454251, deep, taker 0 -> 454251 reported and true
    //
    // The old code picked alpha by one tick. bravo is genuinely cheaper.
    //
    // Note the names: "alpha" sorts before "bravo", so the tie-break introduced
    // for determinism would pick alpha if these were tied. They are not, and
    // this test would not notice the difference if they were confused.
    std::map<std::string, ArrayBook> books;
    ArrayBook alpha(spec());
    alpha.apply(Side::kAsk, Price{453073}, Qty{1'000'000});   // 0.01
    alpha.apply(Side::kAsk, Price{453074}, Qty{99'000'000});  // 0.99
    alpha.set_last_update(kNow);
    books.emplace("alpha", std::move(alpha));
    books.emplace("bravo", ladder(454251, 100, 100'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("alpha", 453072, 453073, 26));
    consolidated.update(quote("bravo", 454250, 454251, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "bravo");
    CHECK(best.effective_vwap == Price{454251});
    CHECK_FALSE(best.execution.depth_exhausted);
}

TEST_CASE("best_execution breaks a tie by venue name", "[consolidated][execution][determinism]") {
    // Same books, same fees, same everything. Without a tie-break the winner is
    // whichever venue was updated first, so two processes reading one market
    // route differently — and truncation used to manufacture these ties out of
    // genuinely different costs.
    std::map<std::string, ArrayBook> books;
    books.emplace("zulu", ladder(452840, 100, 100'000'000));
    books.emplace("alpha", ladder(452840, 100, 100'000'000));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    ConsolidatedBook zulu_first(spec());
    zulu_first.update(quote("zulu", 452839, 452840, 0));
    zulu_first.update(quote("alpha", 452839, 452840, 0));

    ConsolidatedBook alpha_first(spec());
    alpha_first.update(quote("alpha", 452839, 452840, 0));
    alpha_first.update(quote("zulu", 452839, 452840, 0));

    for (const Side side : {Side::kAsk, Side::kBid}) {
        const BestExecution a =
            best_execution(zulu_first, side, Qty{100'000'000}, kNow, lookup);
        const BestExecution b =
            best_execution(alpha_first, side, Qty{100'000'000}, kNow, lookup);
        REQUIRE(a.found);
        REQUIRE(b.found);
        CHECK(a.venue == "alpha");
        CHECK(b.venue == "alpha");
    }
}

TEST_CASE("best_execution sells on the venue that nets the most",
          "[consolidated][execution]") {
    // The entire sell side was untested: every existing case took the ask, so
    // the bid-side fee direction and the "higher effective price wins"
    // comparator had never been executed at all.
    //
    // "highbid" quotes 0.5 higher but charges 50 bps, which on a ~45290 price
    // is ~226 ticks. Selling there nets less.
    std::map<std::string, ArrayBook> books;
    books.emplace("highbid", ladder(452901, 100, 100'000'000));   // bids from 452900
    books.emplace("lowfee", ladder(452896, 100, 100'000'000));    // bids from 452895

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("highbid", 452900, 452901, 50));
    consolidated.update(quote("lowfee", 452895, 452896, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kBid, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "lowfee");
    CHECK(best.execution.vwap == Price{452895});
    CHECK(best.effective_vwap == Price{452895});  // Zero fee: nothing netted out.

    // And the fee really does reduce proceeds on the venue that charges it.
    EffectivePrice high{};
    REQUIRE(consolidated.best(Side::kBid, kNow, high));
    CHECK(high.venue == "lowfee");
}

TEST_CASE("best_execution ranks two partial fills on price", "[consolidated][execution]") {
    // Both venues run out of book. Neither can be preferred on completeness, so
    // the comparison falls through to effective price — a branch no existing
    // case reached, since every one of them had a venue that could fill.
    std::map<std::string, ArrayBook> books;
    books.emplace("cheap", ladder(452800, 1, 1'000'000));
    books.emplace("dear", ladder(452810, 1, 2'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("cheap", 452799, 452800, 0));
    consolidated.update(quote("dear", 452809, 452810, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "cheap");
    CHECK(best.execution.depth_exhausted);  // And the caller is told so.
    CHECK(best.venues_considered == 2);
}

TEST_CASE("best_execution refuses a non-positive size", "[consolidated][execution]") {
    // A zero or negative size is a caller bug, and the honest answer is
    // "nothing", not a venue chosen on a VWAP of zero.
    std::map<std::string, ArrayBook> books;
    books.emplace("a", ladder(452840, 100, 100'000'000));
    books.emplace("b", ladder(452841, 100, 100'000'000));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("a", 452839, 452840, 0));
    consolidated.update(quote("b", 452840, 452841, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    for (const Qty wanted : {Qty{0}, Qty{-100}}) {
        const BestExecution best = best_execution(consolidated, Side::kAsk, wanted, kNow, lookup);
        CHECK_FALSE(best.found);
        CHECK(best.venues_considered == 2);
        CHECK(best.venues_rejected == 2);  // Counted, not silently dropped.
    }
}

TEST_CASE("best_execution refuses a book quoting another instrument",
          "[consolidated][execution][scale]") {
    // THE DEFECT: `best_execution` walked whatever book the lookup returned and
    // never compared `book->spec()` with the consolidated spec — the two are
    // populated on separate paths, so a feed-handler routing bug puts an ETH
    // book behind a BTC venue name and the ~30000 mantissa wins every ask
    // ranking by a factor of fifteen. A confident, enormous, entirely fictional
    // arbitrage.
    std::map<std::string, ArrayBook> books;
    books.emplace("good", ladder(452840, 100, 100'000'000));
    books.emplace("miswired",
                  ladder(300'000, 100, 100'000'000, InstrumentSpec{"ETH/USD", 1, 8}));

    ConsolidatedBook consolidated(spec());
    consolidated.update(quote("good", 452839, 452840, 0));
    consolidated.update(quote("miswired", 452838, 452839, 0));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "good");
    CHECK(best.venues_considered == 1);
    CHECK(best.venues_rejected == 1);
}

TEST_CASE("best_execution refuses a stale book behind a fresh quote",
          "[consolidated][execution][staleness]") {
    // THE DEFECT: freshness was checked on the VenueQuote while the data
    // actually walked came from a BasicL2Book the caller supplied separately.
    // Since this header deliberately holds COPIES of each touch, updating
    // quotes and books on different paths is the natural design — and then the
    // staleness gate protects a struct nobody trades against while the stale
    // book is walked at full size. `BasicL2Book::last_update()` already existed
    // and was never consulted.
    constexpr Timestamp kLater = 20'000'000'000;
    std::map<std::string, ArrayBook> books;
    // Attractive price, book last updated 19 seconds ago against a 5s limit.
    books.emplace("frozen", ladder(452800, 100, 100'000'000, spec(), kNow));
    books.emplace("live", ladder(452840, 100, 100'000'000, spec(), kLater));

    ConsolidatedBook consolidated(spec(), 5'000'000'000);
    consolidated.update(quote("frozen", 452799, 452800, 0, kLater));  // Quote IS fresh.
    consolidated.update(quote("live", 452839, 452840, 0, kLater));

    auto lookup = [&](std::string_view venue) -> const ArrayBook* {
        const auto it = books.find(std::string(venue));
        return it == books.end() ? nullptr : &it->second;
    };

    const BestExecution best =
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kLater, lookup);
    REQUIRE(best.found);
    CHECK(best.venue == "live");
    CHECK(best.venues_considered == 1);
    CHECK(best.venues_rejected == 1);
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
        best_execution(consolidated, Side::kAsk, Qty{100'000'000}, kNow, lookup);
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
