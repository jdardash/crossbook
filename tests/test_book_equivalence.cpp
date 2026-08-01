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
#include <random>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

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

}  // namespace

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

TEST_CASE("ArrayBook keeps the spill map small under realistic churn", "[book][equivalence]") {
    // If the overflow map grows without bound, ArraySide is a std::map with
    // extra steps and its performance claim is void. This asserts the window is
    // actually doing its job, not just producing correct answers.
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});
    for (const Update& u : generate(99, 20000, 500)) {
        subject.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    CHECK(subject.bids().overflow_size() <= ArraySide::kOverflowRebuildThreshold);
    CHECK(subject.asks().overflow_size() <= ArraySide::kOverflowRebuildThreshold);
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
