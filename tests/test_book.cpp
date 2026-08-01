// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "crossbook/book.hpp"

using namespace crossbook;

// Every behavioural test runs against both implementations. A property that
// only holds for the reference is not a property of the library.
#define BOOK_TYPES MapBook, ArrayBook

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
