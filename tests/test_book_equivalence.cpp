// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The differential test that licenses ArraySide to exist.
//
// ArrayBook is the interesting implementation: direct-address slots, a moving
// window, a spill map, and a rebuild path. Every one of those is a place to be
// wrong in a way that only shows up under a specific sequence of updates.
//
// MapBook is boring and obviously correct. So: drive identical randomised event
// streams through both and require their entire observable state to match after
// every single update. Not at the end — after each one, so a divergence is
// reported at the update that caused it rather than a million updates later.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <random>
#include <utility>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

constexpr std::int64_t kI64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kI64Min = (std::numeric_limits<std::int64_t>::min)();

/// Every observable property of a book, in one comparable value.
struct Snapshot {
    std::uint64_t hash{};
    std::uint32_t checksum{};
    std::size_t bid_levels{};
    std::size_t ask_levels{};
    std::vector<Level> top_bids;
    std::vector<Level> top_asks;

    friend bool operator==(const Snapshot&, const Snapshot&) = default;
};

template <typename BookT>
Snapshot snapshot_of(const BookT& book) {
    Snapshot s;
    s.hash = book.state_hash();
    s.checksum = kraken_checksum(book);
    s.bid_levels = book.bids().size();
    s.ask_levels = book.asks().size();
    // 25 is deeper than the checksum's 10, so ordering errors below the
    // checksum horizon still get caught.
    (void)book.top(Side::kBid, 25, s.top_bids);
    (void)book.top(Side::kAsk, 25, s.top_asks);
    return s;
}

struct Update {
    Side side;
    std::int64_t price;
    std::int64_t qty;
};

/// Generate updates clustered near a touch that drifts over time, which is what
/// a real book does and what the moving window is designed for. A uniform
/// spread over the whole price range would never exercise the window at all.
std::vector<Update> generate(std::uint32_t seed, std::size_t count, std::int64_t spread) {
    std::mt19937 rng(seed);
    std::vector<Update> out;
    out.reserve(count);

    std::int64_t touch = 452852;
    for (std::size_t i = 0; i < count; ++i) {
        // Drift the touch, occasionally hard enough to force a window rebuild.
        if ((rng() % 64) == 0) {
            touch += static_cast<std::int64_t>(rng() % 4096) - 2048;
        } else {
            touch += static_cast<std::int64_t>(rng() % 5) - 2;
        }

        const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
        const auto offset = static_cast<std::int64_t>(rng() % static_cast<std::uint32_t>(spread));
        const std::int64_t price = (side == Side::kBid) ? touch - offset : touch + 1 + offset;

        // A quarter of updates delete, matching the churn of a real feed where
        // most messages cancel or replace rather than add.
        const std::int64_t qty = (rng() % 4 == 0) ? 0 : static_cast<std::int64_t>(rng() % 1'000'000) + 1;
        out.push_back(Update{side, price, qty});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Side-level differential, for the part of the input domain the book refuses
// ---------------------------------------------------------------------------
//
// The two generators above draw from a narrow band around 452852, and the
// fuzzer draws exactly `452852 + uint16 - 32768` — a 65536-wide band, which is
// precisely the window size. Between them they cannot construct a price far
// enough from the anchor to make the window's index arithmetic wrap, so the
// int64 wraparound bug lived under a differential test that ran millions of
// updates and reported everything matching.
//
// Closing that hole means driving MapSide and ArraySide directly rather than
// through BasicL2Book, because the book now rejects negative prices at its
// boundary (see the negative-value guard in book.hpp) and a negative price is
// exactly what the far-end wrap needs. ArraySide is a public, independently
// usable container, so its own contract has to hold over the whole int64 range
// whatever the book above it chooses to admit.

/// Every observable property of one side container, in one comparable value.
struct SideSnapshot {
    std::size_t levels{};
    bool has_best{};
    Level best{};
    std::vector<Level> all;

    friend bool operator==(const SideSnapshot&, const SideSnapshot&) = default;
};

template <typename SideT>
SideSnapshot side_snapshot_of(const SideT& s) {
    SideSnapshot snap;
    snap.levels = s.size();
    Level b{};
    snap.has_best = s.best(b);
    if (snap.has_best) {
        snap.best = b;
    }
    s.for_each([&](const Level& lvl) {
        snap.all.push_back(lvl);
        return true;
    });
    return snap;
}

/// `base + delta` with wrap, so a test can name a price near either end of the
/// int64 range without invoking the signed overflow it is testing for.
[[nodiscard]] std::int64_t bump(std::int64_t base, std::int64_t delta) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(base) +
                                     static_cast<std::uint64_t>(delta));
}

/// Prices chosen to straddle every boundary the window arithmetic has: both
/// ends of the int64 range, zero, and the four slots either side of the window
/// edges implied by anchoring on `anchor`.
std::vector<std::int64_t> extreme_prices(std::int64_t anchor) {
    const auto slots = static_cast<std::int64_t>(ArraySide::kDefaultSlots);
    const std::int64_t half = slots / 2;
    return {
        kI64Max,          kI64Max - 1,        kI64Max - 9,
        kI64Max - 10,     kI64Max - half,     kI64Max - half - 1,
        kI64Max - slots,  kI64Max - slots + 1,
        kI64Min,          kI64Min + 1,        kI64Min + 5,
        kI64Min + half,   kI64Min + half - 1, kI64Min + slots,
        -1,               0,                  1,
        bump(anchor, -half - 1),              bump(anchor, -half),
        bump(anchor, -half + 1),              bump(anchor, half - 2),
        bump(anchor, half - 1),               bump(anchor, half),
        bump(anchor, half + 1),
    };
}

}  // namespace

TEST_CASE("ArraySide files a far-end price outside the window, not inside it",
          "[book][equivalence][extremes]") {
    // The exact reproduction from the audit, kept literal because the numbers
    // are the evidence. On a default 65536-slot ask side the old code anchored
    // at base_ = INT64_MAX - 32778 and then computed INT64_MIN+5's offset as a
    // signed subtraction that overflowed and landed on 32784 — inside [0,
    // slots_) — so a price at the opposite end of the int64 range was stored as
    // though it sat 26 ticks from the touch.
    //
    // The assertion that matters is overflow_size(): a release build's
    // out-of-bounds-adjacent write is silent, and "the code ran" proves
    // nothing. A price that cannot be windowed MUST be in the spill list. If it
    // is in the window instead, for_each emits it in index order, which after
    // the wrap is no longer price order, and best() answers with the wrong
    // touch while every diagnostic says the window is doing fine.
    MapSide reference(Side::kAsk);
    ArraySide subject(Side::kAsk);

    const std::pair<std::int64_t, std::int64_t> updates[] = {
        {kI64Max - 10, 1},
        {kI64Min + 5, 2},
        {kI64Max - 9, 3},
    };
    for (const auto& [price, qty] : updates) {
        reference.apply(Price{price}, Qty{qty});
        subject.apply(Price{price}, Qty{qty});
        INFO("price=" << price << " qty=" << qty);
        REQUIRE(side_snapshot_of(reference) == side_snapshot_of(subject));
    }

    // INT64_MIN+5 cannot share a 65536-slot window with INT64_MAX-10. It has to
    // be spilled, and it has to be the best ask, because it is the lowest.
    CHECK(subject.overflow_size() == 1);
    Level touch{};
    REQUIRE(subject.best(touch));
    CHECK(touch.price.ticks == kI64Min + 5);
    CHECK(touch.qty.units == 2);
}

TEST_CASE("ArraySide matches MapSide over the whole int64 price range",
          "[book][equivalence][extremes]") {
    // Anchors chosen so the window sits at the top of the range, at the bottom,
    // and in the middle — the first two are where `ticks - slots_/2` and
    // `ticks - base_` overflow, and the third is the control.
    for (const std::int64_t anchor : {std::int64_t{0}, std::int64_t{1}, std::int64_t{452852},
                                      kI64Max - 10, kI64Min + 5, kI64Max / 2, kI64Min / 2}) {
        for (const Side side : {Side::kBid, Side::kAsk}) {
            INFO("anchor=" << anchor << " side=" << to_string(side));

            MapSide reference(side);
            ArraySide subject(side);

            // Anchor the window first, then walk the extremes past it. Two
            // passes: the first inserts, the second overwrites and deletes, so
            // the removal path sees out-of-window prices too.
            reference.apply(Price{anchor}, Qty{7});
            subject.apply(Price{anchor}, Qty{7});
            REQUIRE(side_snapshot_of(reference) == side_snapshot_of(subject));

            const std::vector<std::int64_t> prices = extreme_prices(anchor);
            for (int pass = 0; pass < 2; ++pass) {
                std::int64_t n = 0;
                for (const std::int64_t price : prices) {
                    ++n;
                    // Pass 0 inserts; pass 1 deletes every third level and
                    // rewrites the rest.
                    const std::int64_t qty = (pass == 1 && (n % 3) == 0) ? 0 : n + pass;
                    reference.apply(Price{price}, Qty{qty});
                    subject.apply(Price{price}, Qty{qty});

                    INFO("pass=" << pass << " price=" << price << " qty=" << qty);
                    REQUIRE(side_snapshot_of(reference) == side_snapshot_of(subject));
                }
            }

            // Nothing may be lost: a price that cannot be windowed belongs in
            // the spill list, so the two counts have to add up to the oracle's.
            CHECK(subject.size() == reference.size());
        }
    }
}

TEST_CASE("ArrayBook matches MapBook at the representable price extremes",
          "[book][equivalence][extremes]") {
    // The same sweep at book level, restricted to non-negative prices because
    // BasicL2Book::apply rejects negative ones outright. That guard is why the
    // wrap is no longer reachable through the book — but it is a second line of
    // defence, not the fix, and this pins the first line staying honest for the
    // half of the range the book does accept.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    const auto slots = static_cast<std::int64_t>(ArraySide::kDefaultSlots);
    const std::vector<std::int64_t> prices = {
        kI64Max,          kI64Max - 1,       kI64Max - 10,    kI64Max - slots / 2,
        kI64Max - slots,  kI64Max - slots - 1, 0,             1,
        slots / 2,        slots,             slots + 1,       452852,
    };

    std::int64_t n = 0;
    for (const Side side : {Side::kAsk, Side::kBid}) {
        for (const std::int64_t price : prices) {
            ++n;
            REQUIRE(reference.apply(side, Price{price}, Qty{n}));
            REQUIRE(subject.apply(side, Price{price}, Qty{n}));
            INFO("side=" << to_string(side) << " price=" << price);
            REQUIRE(snapshot_of(reference) == snapshot_of(subject));
        }
    }
    for (const Side side : {Side::kAsk, Side::kBid}) {
        for (const std::int64_t price : prices) {
            REQUIRE(reference.apply(side, Price{price}, Qty{0}));
            REQUIRE(subject.apply(side, Price{price}, Qty{0}));
            INFO("removing side=" << to_string(side) << " price=" << price);
            REQUIRE(snapshot_of(reference) == snapshot_of(subject));
        }
    }
    CHECK(subject.bids().size() == 0);
    CHECK(subject.asks().size() == 0);
}

TEST_CASE("ArrayBook matches MapBook update for update", "[book][equivalence]") {
    // Several seeds, several spreads. Narrow spreads keep everything inside the
    // window; wide ones force spill and rebuild.
    for (std::uint32_t seed : {1U, 2U, 7U, 42U, 1337U}) {
        for (std::int64_t spread : {8, 200, 5000}) {
            INFO("seed=" << seed << " spread=" << spread);

            MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
            ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

            const auto updates = generate(seed, 4000, spread);
            for (std::size_t i = 0; i < updates.size(); ++i) {
                const Update& u = updates[i];
                reference.apply(u.side, Price{u.price}, Qty{u.qty});
                subject.apply(u.side, Price{u.price}, Qty{u.qty});

                // Compare after every update so the failure points at the
                // update that broke it.
                if (!(snapshot_of(reference) == snapshot_of(subject))) {
                    INFO("diverged at update " << i << " side=" << to_string(u.side)
                                               << " price=" << u.price << " qty=" << u.qty);
                    REQUIRE(false);
                }
            }
        }
    }
}

TEST_CASE("ArrayBook survives prices far outside the initial window", "[book][equivalence]") {
    // A 10x price move is not hypothetical in crypto. The window must follow
    // rather than degrade into an unbounded spill map.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    for (std::int64_t decade = 0; decade < 6; ++decade) {
        const std::int64_t base = 100000 * (decade + 1) * 10;
        for (std::int64_t i = 0; i < 200; ++i) {
            reference.apply(Side::kAsk, Price{base + i}, Qty{i + 1});
            subject.apply(Side::kAsk, Price{base + i}, Qty{i + 1});
            reference.apply(Side::kBid, Price{base - i - 1}, Qty{i + 1});
            subject.apply(Side::kBid, Price{base - i - 1}, Qty{i + 1});
        }
        REQUIRE(snapshot_of(reference) == snapshot_of(subject));
    }
}

TEST_CASE("ArrayBook keeps the spill list small under realistic churn", "[book][equivalence]") {
    // If the spill list grows without bound, ArraySide is a std::map with extra
    // steps and its performance claim is void. This asserts the window is
    // actually doing its job under a book geometry that fits in it — which is
    // the claim, and the only geometry for which the bound below is a property
    // rather than a coincidence. See the degradation test that follows for what
    // happens when the geometry does not fit.
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});
    for (const Update& u : generate(99, 20000, 500)) {
        subject.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    CHECK_FALSE(subject.bids().degraded());
    CHECK_FALSE(subject.asks().degraded());
    CHECK(subject.bids().overflow_size() <= ArraySide::kOverflowRebuildThreshold);
    CHECK(subject.asks().overflow_size() <= ArraySide::kOverflowRebuildThreshold);
}

TEST_CASE("a live range wider than the window degrades visibly and boundedly",
          "[book][equivalence][degraded]") {
    // THIS TEST REPLACES AN ASSERTION THAT WAS NEVER TRUE.
    //
    // The spill-list bound above used to be asserted unconditionally, as though
    // `overflow_size() <= kOverflowRebuildThreshold` were an invariant of the
    // implementation. It is not, and never was: the threshold is a rebuild
    // TRIGGER, and a rebuild can only redistribute levels between the window
    // and the spill list — it cannot make a book narrower than it is. 500 levels
    // at 200-tick spacing span 99800 ticks against a 65536-slot window, so a
    // third of them are outside every possible anchor and the bound is violated
    // by construction. The old CHECK passed only because the generator it ran
    // against happened to produce a book that fits.
    //
    // What IS guaranteed, and what this asserts instead:
    //   1. correctness never degrades — ArrayBook still matches MapBook exactly;
    //   2. the degradation is observable via degraded(), not silent;
    //   3. re-anchoring is bounded. Without hysteresis, every out-of-window
    //      insert past the first spill re-triggered a full O(n) collect-and-
    //      refill, which is the 773 us/apply cliff in book.hpp's comment.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    constexpr std::int64_t kLevels = 500;
    constexpr std::int64_t kSpacing = 200;
    constexpr std::int64_t kFloor = 5'000'000;

    for (std::int64_t i = 0; i < kLevels; ++i) {
        const std::int64_t price = kFloor + i * kSpacing;
        reference.apply(Side::kAsk, Price{price}, Qty{i + 1});
        subject.apply(Side::kAsk, Price{price}, Qty{i + 1});
        if (!(snapshot_of(reference) == snapshot_of(subject))) {
            INFO("diverged building the wide book at level " << i << " price=" << price);
            REQUIRE(false);
        }
    }

    const ArraySide& asks = subject.asks();

    // The documented degradation, asserted rather than described.
    CHECK(asks.degraded());
    CHECK(asks.overflow_size() > ArraySide::kOverflowRebuildThreshold);

    // Hysteresis. Every level past the window edge is a candidate trigger:
    // without the floor, the rebuild count tracks the number of out-of-window
    // inserts (order 270 here). With it, one rebuild buys a whole threshold of
    // further drift, so the count is order (spill / threshold).
    const std::uint64_t rebuilds_after_build = asks.rebuild_count();
    INFO("rebuilds=" << rebuilds_after_build << " spill=" << asks.overflow_size());
    CHECK(rebuilds_after_build <= 16);

    // And churning inside the already-degraded range must not restart the
    // thrash: re-pricing a spilled level is an in-place write, not a re-anchor.
    for (std::int64_t round = 0; round < 4; ++round) {
        for (std::int64_t i = 0; i < kLevels; ++i) {
            const std::int64_t price = kFloor + i * kSpacing;
            reference.apply(Side::kAsk, Price{price}, Qty{i + 1 + round});
            subject.apply(Side::kAsk, Price{price}, Qty{i + 1 + round});
        }
    }
    REQUIRE(snapshot_of(reference) == snapshot_of(subject));
    CHECK(asks.rebuild_count() == rebuilds_after_build);

    // Emptying the book must clear the latch, so a later well-behaved snapshot
    // is not permanently stuck in the degraded mode of the one before it.
    for (std::int64_t i = 0; i < kLevels; ++i) {
        reference.apply(Side::kAsk, Price{kFloor + i * kSpacing}, Qty{0});
        subject.apply(Side::kAsk, Price{kFloor + i * kSpacing}, Qty{0});
    }
    REQUIRE(snapshot_of(reference) == snapshot_of(subject));
    CHECK(asks.overflow_size() == 0);
    CHECK_FALSE(asks.degraded());
}

TEST_CASE("clear resets both implementations identically", "[book][equivalence]") {
    // Applying a fresh snapshot onto stale state is how phantom levels appear
    // after a resync, so clear() has to be total.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    for (const Update& u : generate(5, 500, 100)) {
        reference.apply(u.side, Price{u.price}, Qty{u.qty});
        subject.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    reference.clear();
    subject.clear();

    REQUIRE(snapshot_of(reference) == snapshot_of(subject));
    CHECK(subject.bids().size() == 0);
    CHECK(subject.asks().size() == 0);

    // And they must stay equivalent when reused after the clear.
    for (const Update& u : generate(6, 500, 100)) {
        reference.apply(u.side, Price{u.price}, Qty{u.qty});
        subject.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    REQUIRE(snapshot_of(reference) == snapshot_of(subject));
}
