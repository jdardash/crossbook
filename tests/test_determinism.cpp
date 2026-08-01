// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Determinism.
//
// Replaying a capture must produce byte-identical state every time, on every
// platform, in every build configuration. Without that, a divergence report is
// unfalsifiable: you cannot tell whether the book was wrong or the run was
// merely different.
//
// The mechanism is that there are no floating point values anywhere in the
// book. Prices and quantities are integer mantissas, the hash is integer FNV-1a
// over those mantissas, and iteration order is fully determined by price. There
// is nothing left that could vary by compiler, optimisation level, or CPU.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <random>
#include <utility>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

struct Update {
    Side side;
    std::int64_t price;
    std::int64_t qty;
};

std::vector<Update> canonical_stream(std::size_t count) {
    // Fixed seed and a fixed engine: std::mt19937 is specified by the standard
    // to produce identical output everywhere, unlike the distributions, which
    // are not. Hence the raw modulo rather than std::uniform_int_distribution.
    std::mt19937 rng(20260731U);
    std::vector<Update> out;
    out.reserve(count);
    std::int64_t touch = 452852;
    for (std::size_t i = 0; i < count; ++i) {
        touch += static_cast<std::int64_t>(rng() % 7) - 3;
        const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
        const auto offset = static_cast<std::int64_t>(rng() % 300);
        const std::int64_t price = (side == Side::kBid) ? touch - offset : touch + 1 + offset;
        const std::int64_t qty =
            (rng() % 5 == 0) ? 0 : static_cast<std::int64_t>(rng() % 5'000'000) + 1;
        out.push_back(Update{side, price, qty});
    }
    return out;
}

template <typename BookT>
BookT replay(const std::vector<Update>& stream) {
    BookT book(InstrumentSpec{"BTC/USD", 1, 8});
    for (const Update& u : stream) {
        book.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    return book;
}

}  // namespace

TEST_CASE("replaying the same stream twice gives an identical hash", "[determinism]") {
    const auto stream = canonical_stream(20'000);
    const auto first = replay<ArrayBook>(stream);
    const auto second = replay<ArrayBook>(stream);

    CHECK(first.state_hash() == second.state_hash());
    CHECK(kraken_checksum(first) == kraken_checksum(second));
}

TEST_CASE("both implementations reach the same hash", "[determinism]") {
    // The hash is over observable book state, so it must not depend on which
    // container produced it.
    const auto stream = canonical_stream(20'000);
    CHECK(replay<MapBook>(stream).state_hash() == replay<ArrayBook>(stream).state_hash());
}

TEST_CASE("the hash is order-sensitive across sides", "[determinism]") {
    // A hash that ignored which side a level sat on would call a crossed book
    // identical to a healthy one.
    MapBook a(InstrumentSpec{"BTC/USD", 1, 8});
    MapBook b(InstrumentSpec{"BTC/USD", 1, 8});

    a.apply(Side::kBid, Price{100}, Qty{5});
    b.apply(Side::kAsk, Price{100}, Qty{5});
    CHECK(a.state_hash() != b.state_hash());
}

TEST_CASE("the hash distinguishes price from quantity", "[determinism]") {
    // Concatenating fields without separation lets (price=12, qty=345) collide
    // with (price=123, qty=45). Fixed-width mixing prevents that.
    MapBook a(InstrumentSpec{"BTC/USD", 1, 8});
    MapBook b(InstrumentSpec{"BTC/USD", 1, 8});

    a.apply(Side::kBid, Price{12}, Qty{345});
    b.apply(Side::kBid, Price{123}, Qty{45});
    CHECK(a.state_hash() != b.state_hash());
}

TEST_CASE("an empty book has a stable, non-trivial hash", "[determinism]") {
    MapBook a(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook b(InstrumentSpec{"BTC/USD", 1, 8});
    CHECK(a.state_hash() == b.state_hash());
    CHECK(a.state_hash() != 0);
}

TEST_CASE("insertion order does not affect final state", "[determinism]") {
    // The book is a set of levels, not a log. Two feeds delivering the same
    // levels in different orders must converge, or cross-venue comparison is
    // meaningless.
    auto stream = canonical_stream(5'000);

    // Keep only the last update per (side, price): that is the state the book
    // should hold regardless of arrival order.
    std::map<std::pair<int, std::int64_t>, std::int64_t> final_state;
    for (const Update& u : stream) {
        final_state[{static_cast<int>(u.side), u.price}] = u.qty;
    }

    ArrayBook forward(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook reverse(InstrumentSpec{"BTC/USD", 1, 8});
    for (const auto& [key, qty] : final_state) {
        forward.apply(static_cast<Side>(key.first), Price{key.second}, Qty{qty});
    }
    for (auto it = final_state.rbegin(); it != final_state.rend(); ++it) {
        reverse.apply(static_cast<Side>(it->first.first), Price{it->first.second},
                      Qty{it->second});
    }
    CHECK(forward.state_hash() == reverse.state_hash());
}

TEST_CASE("a book rebuilt after clear matches one built fresh", "[determinism]") {
    // The resync path: clear, then apply a snapshot. Any residue left behind by
    // clear() shows up here as a hash mismatch.
    const auto stream = canonical_stream(5'000);

    ArrayBook reused(InstrumentSpec{"BTC/USD", 1, 8});
    for (const Update& u : canonical_stream(3'000)) {
        reused.apply(u.side, Price{u.price}, Qty{u.qty});
    }
    reused.clear();
    for (const Update& u : stream) {
        reused.apply(u.side, Price{u.price}, Qty{u.qty});
    }

    CHECK(reused.state_hash() == replay<ArrayBook>(stream).state_hash());
}
