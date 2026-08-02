// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

// Every behavioural test runs against both implementations. A property that
// only holds for the reference is not a property of the library.
#define BOOK_TYPES MapBook, ArrayBook

namespace {

/// Do two books agree on everything observable? Level counts, the full ladder
/// down to 64 deep per side, and the determinism hash — the hash alone would
/// do, but when it differs the ladders are what makes the failure readable.
template <typename A, typename B>
bool snapshot_equal(const A& lhs, const B& rhs) {
    std::vector<Level> la;
    std::vector<Level> lb;
    for (const Side s : {Side::kBid, Side::kAsk}) {
        if (lhs.side(s).size() != rhs.side(s).size()) {
            return false;
        }
        if (lhs.top(s, 64, la) != rhs.top(s, 64, lb) || la != lb) {
            return false;
        }
    }
    return lhs.state_hash() == rhs.state_hash();
}

}  // namespace

TEMPLATE_TEST_CASE("a new book is empty", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    CHECK(book.bids().size() == 0);
    CHECK(book.asks().size() == 0);

    Level lvl{};
    CHECK_FALSE(book.best(Side::kBid, lvl));
    CHECK_FALSE(book.best(Side::kAsk, lvl));
}

TEMPLATE_TEST_CASE("levels are ordered best-first", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});

    // Inserted deliberately out of order.
    book.apply(Side::kBid, Price{100}, Qty{1});
    book.apply(Side::kBid, Price{300}, Qty{3});
    book.apply(Side::kBid, Price{200}, Qty{2});
    book.apply(Side::kAsk, Price{500}, Qty{5});
    book.apply(Side::kAsk, Price{400}, Qty{4});
    book.apply(Side::kAsk, Price{600}, Qty{6});

    std::vector<Level> levels;
    REQUIRE(book.top(Side::kBid, 10, levels) == 3);
    CHECK(levels[0] == Level{Price{300}, Qty{3}});  // Bids: high to low.
    CHECK(levels[1] == Level{Price{200}, Qty{2}});
    CHECK(levels[2] == Level{Price{100}, Qty{1}});

    REQUIRE(book.top(Side::kAsk, 10, levels) == 3);
    CHECK(levels[0] == Level{Price{400}, Qty{4}});  // Asks: low to high.
    CHECK(levels[1] == Level{Price{500}, Qty{5}});
    CHECK(levels[2] == Level{Price{600}, Qty{6}});
}

TEMPLATE_TEST_CASE("best returns the touch", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kBid, Price{100}, Qty{1});
    book.apply(Side::kBid, Price{300}, Qty{3});
    book.apply(Side::kAsk, Price{400}, Qty{4});
    book.apply(Side::kAsk, Price{600}, Qty{6});

    Level lvl{};
    REQUIRE(book.best(Side::kBid, lvl));
    CHECK(lvl == Level{Price{300}, Qty{3}});
    REQUIRE(book.best(Side::kAsk, lvl));
    CHECK(lvl == Level{Price{400}, Qty{4}});
}

TEMPLATE_TEST_CASE("quantities are absolute, not deltas", "[book]", BOOK_TYPES) {
    // Every venue covered so far sends the new resting size at a price, not a
    // change to it. Treating these as deltas would accumulate silently.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kBid, Price{100}, Qty{5});
    book.apply(Side::kBid, Price{100}, Qty{3});

    std::vector<Level> levels;
    REQUIRE(book.top(Side::kBid, 10, levels) == 1);
    CHECK(levels[0].qty == Qty{3});
}

TEMPLATE_TEST_CASE("zero quantity removes a level", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kBid, Price{100}, Qty{5});
    book.apply(Side::kBid, Price{200}, Qty{5});
    REQUIRE(book.bids().size() == 2);

    book.apply(Side::kBid, Price{200}, Qty{0});
    CHECK(book.bids().size() == 1);

    Level lvl{};
    REQUIRE(book.best(Side::kBid, lvl));
    CHECK(lvl.price == Price{100});
}

TEMPLATE_TEST_CASE("removing a level that does not exist is a no-op", "[book]", BOOK_TYPES) {
    // Venues do send deletes for levels already gone, especially around a
    // resync. This must not corrupt state or trip a level count.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kBid, Price{100}, Qty{0});
    CHECK(book.bids().size() == 0);

    book.apply(Side::kBid, Price{100}, Qty{5});
    book.apply(Side::kBid, Price{999}, Qty{0});
    CHECK(book.bids().size() == 1);
}

TEMPLATE_TEST_CASE("top respects the requested depth", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < 50; ++i) {
        book.apply(Side::kAsk, Price{1000 + i}, Qty{i + 1});
    }

    std::vector<Level> levels;
    CHECK(book.top(Side::kAsk, 5, levels) == 5);
    CHECK(levels.size() == 5);
    CHECK(levels.front().price == Price{1000});
    CHECK(levels.back().price == Price{1004});

    CHECK(book.top(Side::kAsk, 100, levels) == 50);  // Clamped to what exists.
}

TEMPLATE_TEST_CASE("sides are independent", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    book.apply(Side::kBid, Price{100}, Qty{1});
    book.apply(Side::kAsk, Price{100}, Qty{2});  // Same price, other side.

    CHECK(book.bids().size() == 1);
    CHECK(book.asks().size() == 1);

    book.apply(Side::kBid, Price{100}, Qty{0});
    CHECK(book.bids().size() == 0);
    CHECK(book.asks().size() == 1);
}

TEMPLATE_TEST_CASE("clear empties both sides", "[book]", BOOK_TYPES) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < 20; ++i) {
        book.apply(Side::kBid, Price{1000 - i}, Qty{i + 1});
        book.apply(Side::kAsk, Price{1001 + i}, Qty{i + 1});
    }
    book.clear();
    CHECK(book.bids().size() == 0);
    CHECK(book.asks().size() == 0);

    Level lvl{};
    CHECK_FALSE(book.best(Side::kBid, lvl));
    CHECK_FALSE(book.best(Side::kAsk, lvl));
}

TEMPLATE_TEST_CASE("the instrument spec is retained", "[book]", BOOK_TYPES) {
    // The scales are what make the checksum reproducible, so losing them
    // silently would be a correctness bug rather than a cosmetic one.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    CHECK(book.spec().symbol == "BTC/USD");
    CHECK(book.spec().price_scale == 1);
    CHECK(book.spec().qty_scale == 8);
}

TEMPLATE_TEST_CASE("top clamps a caller-supplied depth before reserving", "[book]", BOOK_TYPES) {
    // `top(side, SIZE_MAX, out)` means "give me everything". It used to mean
    // "reserve 2^64 Levels", which throws std::length_error out of what reads
    // like a plain accessor — a read of the book taking the process down
    // because of an argument that asked for no more than the book already has.
    // Depth arguments come from configuration and from callers doing arithmetic
    // on level counts, so an unbounded one is not an exotic input.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < 5; ++i) {
        book.apply(Side::kAsk, Price{1000 + i}, Qty{i + 1});
    }

    std::vector<Level> out;
    std::size_t got = 0;
    REQUIRE_NOTHROW(got = book.top(Side::kAsk, std::numeric_limits<std::size_t>::max(), out));
    CHECK(got == 5);
    CHECK(out.size() == 5);
    CHECK(out.front().price == Price{1000});
    CHECK(out.back().price == Price{1004});

    // The same on an empty side: reserve(0) is fine, but the clamp must not
    // turn "everything" into "nothing" when there is something.
    std::size_t none = 1;
    REQUIRE_NOTHROW(none = book.top(Side::kBid, std::numeric_limits<std::size_t>::max(), out));
    CHECK(none == 0);
}

TEMPLATE_TEST_CASE("negative price and negative quantity are rejected", "[book]", BOOK_TYPES) {
    // Removal keys on is_zero(), not on sign, so a negative quantity used to be
    // stored as a live level. Rejecting it has to leave the book EXACTLY as it
    // was: a rejected update that half-applied would be worse than the bug.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    CHECK(book.apply(Side::kBid, Price{100}, Qty{5}));

    CHECK_FALSE(book.apply(Side::kBid, Price{100}, Qty{-5}));
    CHECK(book.bids().size() == 1);
    Level lvl{};
    REQUIRE(book.best(Side::kBid, lvl));
    CHECK(lvl == Level{Price{100}, Qty{5}});  // Untouched, not overwritten.

    CHECK_FALSE(book.apply(Side::kBid, Price{-100}, Qty{5}));
    CHECK(book.bids().size() == 1);
    CHECK_FALSE(book.apply(Side::kAsk, Price{-1}, Qty{-1}));
    CHECK(book.asks().size() == 0);

    // Zero is a removal, not a rejection — the distinction the guard must not
    // blur, since every venue deletes levels by sending a size of zero.
    CHECK(book.apply(Side::kBid, Price{100}, Qty{0}));
    CHECK(book.bids().size() == 0);
}

TEST_CASE("a sign-flipped quantity cannot pass the venue checksum", "[book][checksum]") {
    // WHY THE GUARD IS WORTH A RETURN VALUE AND NOT JUST A COMMENT.
    //
    // Kraken's checksum payload is the mantissa's decimal digits, and
    // write_digits emits the MAGNITUDE. So a level of -500000 produced byte-
    // identical payload bytes to +500000 and hashed to the same CRC32. A
    // decoder sign bug therefore yielded a book that was wrong about resting
    // size while still agreeing with the exchange's own proof of correctness —
    // a false PASS from the one mechanism this library exists to provide.
    ArrayBook good(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook bad(InstrumentSpec{"BTC/USD", 1, 8});

    REQUIRE(good.apply(Side::kAsk, Price{452853}, Qty{500000}));
    // Non-fatal on purpose: if the guard ever regresses, the checksum equality
    // below is the evidence worth seeing, and a REQUIRE here would hide it.
    CHECK_FALSE(bad.apply(Side::kAsk, Price{452853}, Qty{-500000}));

    CHECK(bad.asks().size() == 0);
    CHECK(kraken_checksum(good) != kraken_checksum(bad));
    CHECK(good.state_hash() != bad.state_hash());
}

TEST_CASE("clear is total after the window has re-anchored", "[book][clear]") {
    // The watermarks that keep clear() off the full window are per-anchor
    // state, and a rebuild moves the base out from under them. A watermark that
    // does not follow the re-anchor leaves live slots behind, and a slot left
    // behind across a resync is a phantom level — the exact failure clear()
    // exists to prevent, arriving minutes later as a checksum mismatch on an
    // update that did nothing wrong.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    // Walk the touch across several windows so the base moves repeatedly, and
    // leave levels at both ends of each window along the way.
    constexpr std::int64_t kDepth = 64;
    std::int64_t lo = 3'000'000;
    for (std::int64_t i = 0; i < kDepth; ++i) {
        reference.apply(Side::kAsk, Price{lo + i}, Qty{i + 1});
        subject.apply(Side::kAsk, Price{lo + i}, Qty{i + 1});
    }
    for (std::int64_t s = 0; s < 150'000; ++s) {
        const std::int64_t add = lo + kDepth;
        reference.apply(Side::kAsk, Price{add}, Qty{(s % 97) + 1});
        subject.apply(Side::kAsk, Price{add}, Qty{(s % 97) + 1});
        reference.apply(Side::kAsk, Price{lo}, Qty{0});
        subject.apply(Side::kAsk, Price{lo}, Qty{0});
        ++lo;
    }
    REQUIRE(subject.asks().rebuild_count() > 0);  // The re-anchor really happened.
    REQUIRE(snapshot_equal(reference, subject));

    reference.clear();
    subject.clear();
    CHECK(subject.asks().size() == 0);
    CHECK(subject.asks().overflow_size() == 0);
    Level lvl{};
    CHECK_FALSE(subject.best(Side::kAsk, lvl));
    REQUIRE(snapshot_equal(reference, subject));

    // Reuse at a completely different price region: a stale slot from before the
    // clear would surface here as a level nobody sent.
    for (std::int64_t i = 0; i < 40; ++i) {
        reference.apply(Side::kAsk, Price{777'000 + i}, Qty{i + 1});
        subject.apply(Side::kAsk, Price{777'000 + i}, Qty{i + 1});
        reference.apply(Side::kBid, Price{776'999 - i}, Qty{i + 1});
        subject.apply(Side::kBid, Price{776'999 - i}, Qty{i + 1});
    }
    REQUIRE(snapshot_equal(reference, subject));
    CHECK(subject.asks().size() == 40);
}

TEST_CASE("clear costs the occupied span, not the whole window", "[book][clear]") {
    // clear() used to memset both sides in full: 2 x 65536 slots x 8 bytes, one
    // mebibyte, ~52 us, and it evicts L2 on the way through. The comment on it
    // said "clear() is not hot". feed.hpp calls book_.clear() on EVERY resync —
    // which is precisely the moment the book is behind the venue and racing to
    // catch up, and precisely the moment 52 us of stores to slots that were
    // already zero, plus a cold cache afterwards, costs the most.
    //
    // Asserted as a RATIO against the work the old implementation did, measured
    // on the same machine inside the same loop, because an absolute latency
    // bound in a test is a flake waiting for a busy CI box. The second loop is
    // the first loop plus exactly the two full-window fills that clear() used to
    // perform. A correct clear() is several hundred times cheaper than those
    // fills; the old one WAS those fills, so its ratio was about 2. The bar is 4.
    constexpr std::size_t kIters = 500;
    constexpr std::int64_t kTouch = 452852;

    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});
    auto populate = [&book](std::int64_t i) {
        for (std::int64_t k = 0; k < 10; ++k) {
            book.apply(Side::kBid, Price{kTouch - k}, Qty{i + k + 1});
            book.apply(Side::kAsk, Price{kTouch + 1 + k}, Qty{i + k + 1});
        }
    };

    std::vector<std::int64_t> mimic_bids(ArraySide::kDefaultSlots, 0);
    std::vector<std::int64_t> mimic_asks(ArraySide::kDefaultSlots, 0);
    std::int64_t sink = 0;

    // Warm both paths so neither pays for a cold page on its first iteration.
    populate(0);
    book.clear();
    std::fill(mimic_bids.begin(), mimic_bids.end(), std::int64_t{1});
    std::fill(mimic_asks.begin(), mimic_asks.end(), std::int64_t{1});

    const auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        populate(static_cast<std::int64_t>(i));
        book.clear();
    }
    const auto t1 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < kIters; ++i) {
        populate(static_cast<std::int64_t>(i));
        book.clear();
        const auto v = static_cast<std::int64_t>(i) + 1;
        std::fill(mimic_bids.begin(), mimic_bids.end(), v);
        std::fill(mimic_asks.begin(), mimic_asks.end(), v);
        // Read the buffers back so neither fill can be optimised away.
        sink += mimic_bids[i % mimic_bids.size()] + mimic_asks[i % mimic_asks.size()];
    }
    const auto t2 = std::chrono::steady_clock::now();

    const double clear_only = std::chrono::duration<double>(t1 - t0).count();
    const double clear_plus_full_fill = std::chrono::duration<double>(t2 - t1).count();
    const double ratio = clear_plus_full_fill / std::max(clear_only, 1e-9);

    CHECK(sink > 0);
    INFO("clear-only " << clear_only << "s, clear+full-fill " << clear_plus_full_fill
                       << "s, ratio " << ratio);
    CHECK(ratio > 4.0);
}

TEST_CASE("precedes encodes book ordering", "[book]") {
    CHECK(precedes(Side::kBid, 200, 100));       // Higher bid is better.
    CHECK_FALSE(precedes(Side::kBid, 100, 200));
    CHECK(precedes(Side::kAsk, 100, 200));       // Lower ask is better.
    CHECK_FALSE(precedes(Side::kAsk, 200, 100));
}

TEST_CASE("opposite flips the side", "[book]") {
    CHECK(opposite(Side::kBid) == Side::kAsk);
    CHECK(opposite(Side::kAsk) == Side::kBid);
}
