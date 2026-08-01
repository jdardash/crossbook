// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Depth trimming.
//
// This exists because of a bug that live verification found and no unit test
// would have. Subscribed to Kraken BTC/USD at depth 10, the book grew to 20 bid
// levels over a minute and 4 of 298 checksums failed.
//
// The cause is a gap in the depth-limited contract that is easy to miss: the
// venue reports cancellations, so a naive reader looks correct, but it never
// reports that a level fell out of the window because a better one arrived —
// from its side there is nothing to say. Those orphaned levels sit below the
// checksummed depth doing no harm until removals near the touch promote one back
// into the top ten, and then the checksum fails on an update that was itself
// perfectly fine.
//
// The tests below pin the behaviour; `test_fixture_replay.cpp` pins the
// end-to-end consequence against the recorded capture.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "crossbook/book.hpp"

using namespace crossbook;

namespace {

template <typename BookT>
std::vector<Level> levels(const BookT& book, Side side) {
    std::vector<Level> out;
    book.side(side).for_each([&](const Level& level) {
        out.push_back(level);
        return true;
    });
    return out;
}

}  // namespace

TEMPLATE_TEST_CASE("Trimming keeps the levels nearest the touch", "[trim]", MapBook, ArrayBook) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});

    // Bids descend from the touch, asks ascend.
    for (int i = 0; i < 20; ++i) {
        book.apply(Side::kBid, Price{630000 - i}, Qty{100 + i});
        book.apply(Side::kAsk, Price{630010 + i}, Qty{200 + i});
    }
    REQUIRE(book.bids().size() == 20);
    REQUIRE(book.asks().size() == 20);

    CHECK(book.trim(10) == 20);  // Ten dropped from each side.
    REQUIRE(book.bids().size() == 10);
    REQUIRE(book.asks().size() == 10);

    const auto bids = levels(book, Side::kBid);
    CHECK(bids.front().price == Price{630000});   // Best bid survives.
    CHECK(bids.back().price == Price{630000 - 9});
    const auto asks = levels(book, Side::kAsk);
    CHECK(asks.front().price == Price{630010});
    CHECK(asks.back().price == Price{630010 + 9});
}

TEMPLATE_TEST_CASE("Trimming a book already within depth changes nothing", "[trim]", MapBook,
                   ArrayBook) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (int i = 0; i < 5; ++i) {
        book.apply(Side::kBid, Price{630000 - i}, Qty{1});
    }
    const std::uint64_t before = book.state_hash();
    CHECK(book.trim(10) == 0);
    CHECK(book.state_hash() == before);
}

TEMPLATE_TEST_CASE("A depth of zero means a full book and never trims", "[trim]", MapBook,
                   ArrayBook) {
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (int i = 0; i < 50; ++i) {
        book.apply(Side::kBid, Price{630000 - i}, Qty{1});
    }
    CHECK(book.trim(0) == 0);
    CHECK(book.bids().size() == 50);
}

TEMPLATE_TEST_CASE("Trimming more levels than one batch can name still converges", "[trim]",
                   MapBook, ArrayBook) {
    // The removal buffer is a fixed 64 entries, so a book this far over depth
    // needs several passes. Getting the loop wrong leaves a book that is quietly
    // still too deep, which is the exact failure being fixed.
    TestType book(InstrumentSpec{"BTC/USD", 1, 8});
    for (int i = 0; i < 500; ++i) {
        book.apply(Side::kBid, Price{630000 - i}, Qty{1});
    }
    CHECK(book.trim(Side::kBid, 10) == 490);
    CHECK(book.bids().size() == 10);
    CHECK(levels(book, Side::kBid).front().price == Price{630000});
}

TEST_CASE("Both book implementations trim identically", "[trim]") {
    // The differential oracle again: the array book is the one that could be
    // wrong, and trimming touches its window and its overflow map at once.
    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});

    // Prices deliberately spread far enough to push some levels out of the
    // array's window and into its overflow map.
    for (int i = 0; i < 200; ++i) {
        const std::int64_t price = 630000 - (i * 977 % 40000);
        reference.apply(Side::kBid, Price{price}, Qty{1 + i});
        subject.apply(Side::kBid, Price{price}, Qty{1 + i});
    }
    REQUIRE(reference.state_hash() == subject.state_hash());

    CHECK(reference.trim(Side::kBid, 10) == subject.trim(Side::kBid, 10));
    CHECK(reference.bids().size() == subject.bids().size());
    CHECK(reference.state_hash() == subject.state_hash());
}

TEST_CASE("Trimming removes exactly the levels a top-N view would not show", "[trim]") {
    // The scenario from the live failure, in miniature: a book at depth, then a
    // new best arrives. The venue sends no removal for the level pushed out, so
    // without trimming the book carries N+1 levels and the checksum input is
    // wrong the moment the extra one is promoted back into view.
    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});
    for (int i = 0; i < 10; ++i) {
        book.apply(Side::kAsk, Price{630100 + i}, Qty{5});
    }
    REQUIRE(book.asks().size() == 10);

    book.apply(Side::kAsk, Price{630099}, Qty{7});  // A new best ask.
    CHECK(book.asks().size() == 11);

    CHECK(book.trim(Side::kAsk, 10) == 1);
    const auto asks = levels(book, Side::kAsk);
    REQUIRE(asks.size() == 10);
    CHECK(asks.front().price == Price{630099});
    CHECK(asks.back().price == Price{630108});  // 630109 is gone, as the venue sees it.
}
