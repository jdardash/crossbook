// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <random>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/l3.hpp"

using namespace crossbook;

namespace {
InstrumentSpec spec() { return InstrumentSpec{"BTC/USD", 1, 8}; }
}  // namespace

TEST_CASE("a new L3 book is empty", "[l3]") {
    L3Book book(spec());
    CHECK(book.order_count() == 0);
    Level lvl{};
    CHECK_FALSE(book.best(Side::kBid, lvl));
    CHECK(book.find(1) == nullptr);
}

TEST_CASE("orders aggregate into levels", "[l3]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));
    REQUIRE(book.add(3, Side::kBid, Price{452834}, Qty{500}));

    CHECK(book.order_count() == 3);
    const L3Level* touch = book.find_level(Side::kBid, Price{452835});
    REQUIRE(touch != nullptr);
    CHECK(touch->qty == Qty{350});
    CHECK(touch->orders == 2);

    Level best{};
    REQUIRE(book.best(Side::kBid, best));
    CHECK(best.price == Price{452835});
    CHECK(best.qty == Qty{350});
}

TEST_CASE("a duplicate order id is rejected", "[l3]") {
    // A venue re-adding a live id is a protocol violation. Treating it as an
    // update would corrupt the level aggregate in a way nothing downstream
    // could detect.
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    CHECK_FALSE(book.add(1, Side::kBid, Price{452835}, Qty{999}));

    const L3Level* level = book.find_level(Side::kBid, Price{452835});
    REQUIRE(level != nullptr);
    CHECK(level->qty == Qty{100});
}

TEST_CASE("non-positive quantities are rejected on add", "[l3]") {
    L3Book book(spec());
    CHECK_FALSE(book.add(1, Side::kBid, Price{452835}, Qty{0}));
    CHECK_FALSE(book.add(2, Side::kBid, Price{452835}, Qty{-5}));
    CHECK(book.order_count() == 0);
}

TEST_CASE("removing empties the level rather than leaving a phantom", "[l3]") {
    // A level left behind with zero orders would show up in the L2 projection
    // as a price that does not exist.
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.remove(1));

    CHECK(book.order_count() == 0);
    CHECK(book.find_level(Side::kBid, Price{452835}) == nullptr);
    Level lvl{};
    CHECK_FALSE(book.best(Side::kBid, lvl));
}

TEST_CASE("removing an unknown order is a no-op", "[l3]") {
    L3Book book(spec());
    CHECK_FALSE(book.remove(999));
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    CHECK_FALSE(book.remove(999));
    CHECK(book.order_count() == 1);
}

// ---------------------------------------------------------------------------
// Queue position — the reason L3 exists
// ---------------------------------------------------------------------------

TEST_CASE("queue_ahead reports volume resting in front", "[l3][queue]") {
    // Not derivable from aggregated depth at any resolution. This is the number
    // that decides whether a resting strategy works.
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));
    REQUIRE(book.add(3, Side::kBid, Price{452835}, Qty{50}));

    Qty ahead{};
    REQUIRE(book.queue_ahead(1, ahead));
    CHECK(ahead == Qty{0});  // First in line.
    REQUIRE(book.queue_ahead(2, ahead));
    CHECK(ahead == Qty{100});
    REQUIRE(book.queue_ahead(3, ahead));
    CHECK(ahead == Qty{350});
}

TEST_CASE("queue_ahead is unavailable for an unknown order", "[l3][queue]") {
    L3Book book(spec());
    Qty ahead{};
    CHECK_FALSE(book.queue_ahead(42, ahead));
}

TEST_CASE("a cancel ahead of you improves your queue position", "[l3][queue]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));
    REQUIRE(book.add(3, Side::kBid, Price{452835}, Qty{50}));

    REQUIRE(book.remove(1));

    Qty ahead{};
    REQUIRE(book.queue_ahead(3, ahead));
    CHECK(ahead == Qty{250});  // Was 350.
}

TEST_CASE("a reduction keeps queue priority", "[l3][queue]") {
    // THE SEMANTIC THAT MUST NOT BE BACKWARDS.
    //
    // Every venue that matters preserves priority on a size reduction. Getting
    // this wrong silently inflates every fill estimate a strategy makes.
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));
    REQUIRE(book.add(3, Side::kBid, Price{452835}, Qty{50}));

    REQUIRE(book.modify(2, Qty{100}));  // 250 -> 100.

    Qty ahead{};
    REQUIRE(book.queue_ahead(2, ahead));
    CHECK(ahead == Qty{100});  // Still second: priority retained.
    REQUIRE(book.queue_ahead(3, ahead));
    CHECK(ahead == Qty{200});

    const L3Level* level = book.find_level(Side::kBid, Price{452835});
    REQUIRE(level != nullptr);
    CHECK(level->qty == Qty{250});
}

TEST_CASE("an increase forfeits queue priority", "[l3][queue]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));
    REQUIRE(book.add(3, Side::kBid, Price{452835}, Qty{50}));

    REQUIRE(book.modify(2, Qty{400}));  // 250 -> 400: goes to the back.

    Qty ahead{};
    REQUIRE(book.queue_ahead(2, ahead));
    CHECK(ahead == Qty{150});  // Now behind orders 1 and 3.
    REQUIRE(book.queue_ahead(3, ahead));
    CHECK(ahead == Qty{100});

    const L3Level* level = book.find_level(Side::kBid, Price{452835});
    REQUIRE(level != nullptr);
    CHECK(level->qty == Qty{550});
}

TEST_CASE("a partial fill always keeps priority", "[l3][queue]") {
    // Distinct from modify: a fill can never cost you priority, whereas a
    // resize to the same quantity might have.
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.add(2, Side::kBid, Price{452835}, Qty{250}));

    REQUIRE(book.execute(1, Qty{60}));

    Qty ahead{};
    REQUIRE(book.queue_ahead(2, ahead));
    CHECK(ahead == Qty{40});  // Order 1 still in front, now smaller.
    CHECK(book.find(1)->qty == Qty{40});
}

TEST_CASE("a full fill removes the order", "[l3]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.execute(1, Qty{100}));
    CHECK(book.order_count() == 0);
    CHECK(book.find(1) == nullptr);
}

TEST_CASE("over-filling removes rather than going negative", "[l3]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.execute(1, Qty{999}));
    CHECK(book.order_count() == 0);
    CHECK(book.find_level(Side::kBid, Price{452835}) == nullptr);
}

TEST_CASE("modify to zero removes the order", "[l3]") {
    L3Book book(spec());
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{100}));
    REQUIRE(book.modify(1, Qty{0}));
    CHECK(book.order_count() == 0);
}

// ---------------------------------------------------------------------------
// Ordering and projection
// ---------------------------------------------------------------------------

TEST_CASE("levels project in book order", "[l3]") {
    L3Book book(spec());
    book.add(1, Side::kBid, Price{100}, Qty{1});
    book.add(2, Side::kBid, Price{300}, Qty{3});
    book.add(3, Side::kBid, Price{200}, Qty{2});
    book.add(4, Side::kAsk, Price{500}, Qty{5});
    book.add(5, Side::kAsk, Price{400}, Qty{4});

    std::vector<Level> bids;
    book.for_each_level(Side::kBid, [&](const Level& l) {
        bids.push_back(l);
        return true;
    });
    REQUIRE(bids.size() == 3);
    CHECK(bids[0].price == Price{300});  // High to low.
    CHECK(bids[2].price == Price{100});

    std::vector<Level> asks;
    book.for_each_level(Side::kAsk, [&](const Level& l) {
        asks.push_back(l);
        return true;
    });
    REQUIRE(asks.size() == 2);
    CHECK(asks[0].price == Price{400});  // Low to high.
}

TEST_CASE("orders iterate in queue order", "[l3]") {
    L3Book book(spec());
    book.add(10, Side::kBid, Price{452835}, Qty{1});
    book.add(20, Side::kBid, Price{452835}, Qty{2});
    book.add(30, Side::kBid, Price{452835}, Qty{3});

    std::vector<OrderId> seen;
    book.for_each_order(Side::kBid, Price{452835}, [&](const Order& o) {
        seen.push_back(o.id);
        return true;
    });
    CHECK(seen == std::vector<OrderId>{10, 20, 30});
}

TEST_CASE("the L2 projection matches an equivalent L2 book", "[l3][equivalence]") {
    // The aggregate is derived rather than maintained in parallel, so the two
    // cannot drift. This checks the derivation itself against the independently
    // implemented L2 book.
    std::mt19937 rng(20260801U);
    L3Book l3(spec());
    MapBook l2(spec());

    std::vector<OrderId> live;
    std::map<std::pair<int, std::int64_t>, std::int64_t> aggregate;

    for (int i = 0; i < 3000; ++i) {
        const bool removing = !live.empty() && (rng() % 3 == 0);
        if (removing) {
            const std::size_t pick = rng() % live.size();
            const OrderId id = live[pick];
            const Order* order = l3.find(id);
            REQUIRE(order != nullptr);

            const auto key = std::make_pair(static_cast<int>(order->side), order->price.ticks);
            aggregate[key] -= order->qty.units;
            l2.apply(order->side, order->price, Qty{aggregate[key]});

            REQUIRE(l3.remove(id));
            live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
        } else {
            const auto id = static_cast<OrderId>(i) + 1;
            const Side side = (rng() % 2) ? Side::kBid : Side::kAsk;
            const std::int64_t price = 452800 + static_cast<std::int64_t>(rng() % 80);
            const std::int64_t qty = static_cast<std::int64_t>(rng() % 1000) + 1;

            REQUIRE(l3.add(id, side, Price{price}, Qty{qty}));
            live.push_back(id);

            const auto key = std::make_pair(static_cast<int>(side), price);
            aggregate[key] += qty;
            l2.apply(side, Price{price}, Qty{aggregate[key]});
        }
    }

    for (const Side side : {Side::kBid, Side::kAsk}) {
        std::vector<Level> from_l3;
        l3.for_each_level(side, [&](const Level& l) {
            from_l3.push_back(l);
            return true;
        });
        std::vector<Level> from_l2;
        (void)l2.top(side, 10'000, from_l2);
        INFO("side=" << to_string(side));
        CHECK(from_l3 == from_l2);
    }
}

// ---------------------------------------------------------------------------
// Arena and hash table behaviour
// ---------------------------------------------------------------------------

TEST_CASE("the arena recycles freed slots", "[l3][arena]") {
    // A pool that only ever grows is a leak wearing a hat.
    L3Book book(spec(), 64);
    for (OrderId id = 1; id <= 500; ++id) {
        REQUIRE(book.add(id, Side::kBid, Price{452835}, Qty{10}));
    }
    const std::size_t peak = book.arena_size();

    for (OrderId id = 1; id <= 500; ++id) {
        REQUIRE(book.remove(id));
    }
    CHECK(book.free_slots() == peak);

    for (OrderId id = 1001; id <= 1500; ++id) {
        REQUIRE(book.add(id, Side::kBid, Price{452835}, Qty{10}));
    }
    CHECK(book.arena_size() == peak);  // Reused, not regrown.
}

TEST_CASE("the id table survives heavy churn", "[l3][arena]") {
    // Tombstones from repeated insert/erase can silently break probe chains,
    // and the failure mode is a live order becoming unfindable.
    L3Book book(spec(), 32);
    for (int round = 0; round < 40; ++round) {
        for (OrderId i = 0; i < 200; ++i) {
            const OrderId id = static_cast<OrderId>(round) * 1000 + i + 1;
            REQUIRE(book.add(id, Side::kAsk, Price{452840 + static_cast<std::int64_t>(i % 20)},
                             Qty{5}));
        }
        for (OrderId i = 0; i < 200; ++i) {
            const OrderId id = static_cast<OrderId>(round) * 1000 + i + 1;
            REQUIRE(book.find(id) != nullptr);
            REQUIRE(book.remove(id));
        }
        REQUIRE(book.order_count() == 0);
    }
}

TEST_CASE("sequential ids do not cluster into a probe storm", "[l3][arena]") {
    // Venue order ids are frequently sequential; a raw modulo would pile every
    // one of them into adjacent slots. This passes either way but would crawl
    // without the mixing step.
    L3Book book(spec(), 1024);
    for (OrderId id = 1; id <= 20'000; ++id) {
        REQUIRE(book.add(id, Side::kBid, Price{452835 - static_cast<std::int64_t>(id % 50)},
                         Qty{1}));
    }
    for (OrderId id = 1; id <= 20'000; ++id) {
        REQUIRE(book.find(id) != nullptr);
    }
    CHECK(book.order_count() == 20'000);
}

TEST_CASE("clear resets everything", "[l3]") {
    L3Book book(spec());
    for (OrderId id = 1; id <= 100; ++id) {
        REQUIRE(book.add(id, Side::kBid, Price{452835}, Qty{10}));
    }
    book.clear();

    CHECK(book.order_count() == 0);
    CHECK(book.find(1) == nullptr);
    Level lvl{};
    CHECK_FALSE(book.best(Side::kBid, lvl));

    // And it is usable afterwards, which is what the resync path needs.
    REQUIRE(book.add(1, Side::kBid, Price{452835}, Qty{10}));
    CHECK(book.order_count() == 1);
}
