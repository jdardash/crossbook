// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// Differential fuzzing: ArrayBook against MapBook.
//
// The equivalence test in tests/ drives randomised streams shaped like a real
// book, which is the realistic case. This drives adversarial streams, which is
// the case that finds bugs: coverage-guided mutation will happily construct the
// exact sequence of window excursions, mass deletes, and re-anchors that a
// hand-written generator never would.
//
// The oracle is total. After every update, the full observable state of both
// books must be identical: hash, checksum, level counts, and the top 32 levels
// per side. Any divergence at all aborts.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "fuzz_check.hpp"

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"

using namespace crossbook;

namespace {

constexpr std::int64_t kI64Max = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kI64Min = (std::numeric_limits<std::int64_t>::min)();
constexpr auto kSlots = static_cast<std::int64_t>(ArraySide::kDefaultSlots);

/// `base + delta` with wrap, so a generator can name a price near either end of
/// the int64 range without committing the signed overflow it exists to test for.
[[nodiscard]] std::int64_t bump(std::int64_t base, std::int64_t delta) noexcept {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(base) +
                                     static_cast<std::uint64_t>(delta));
}

/// Anchors the banded generator below cannot reach.
///
/// The band is 65536 wide, which is EXACTLY the window size, so no pair of
/// prices it produces can ever be far enough apart to make the window's index
/// arithmetic wrap. That is why an int64 wraparound in ArraySide survived a
/// differential fuzzer: the oracle was total, and the input domain excluded the
/// bug. These are the anchors that do reach it.
constexpr std::int64_t kExtremeAnchors[] = {
    kI64Max, kI64Max - kSlots / 2, kI64Max - kSlots,   kI64Min,     kI64Min + kSlots / 2,
    kI64Min + kSlots, 0,           kSlots / 2,         kSlots,      452852,
};

/// Pulls structured updates out of a flat byte string.
class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

    [[nodiscard]] bool exhausted() const noexcept { return pos_ >= size_; }

    [[nodiscard]] std::uint8_t byte() noexcept { return pos_ < size_ ? data_[pos_++] : 0; }

    [[nodiscard]] std::int64_t price() noexcept {
        // Two bytes of offset around a fixed anchor keeps prices in a range
        // where the window is actually exercised, rather than scattering them
        // so widely that everything lands in the spill list.
        const std::int64_t lo = byte();
        const std::int64_t hi = byte();
        return 452852 + ((hi << 8) | lo) - 32768;
    }

    /// A price near one of the int64 extremes, nudged by up to a window edge so
    /// mutation can walk it across the window boundary a slot at a time.
    [[nodiscard]] std::int64_t extreme_price() noexcept {
        constexpr std::size_t n = sizeof(kExtremeAnchors) / sizeof(kExtremeAnchors[0]);
        const std::int64_t anchor = kExtremeAnchors[byte() % n];
        const std::int64_t lo = byte();
        const std::int64_t hi = byte();
        return bump(anchor, ((hi << 8) | lo) - 32768);
    }

    /// Non-negative, because BasicL2Book::apply rejects negative prices at its
    /// boundary; a negative one would compare equal on both books by being
    /// refused by both, which tests the guard rather than the window.
    [[nodiscard]] std::int64_t nonnegative_extreme_price() noexcept {
        const std::int64_t p = extreme_price();
        return (p < 0) ? -(p + 1) : p;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_{0};
};

void require_equivalent(const MapBook& reference, const ArrayBook& subject) {
    CB_CHECK(reference.state_hash() == subject.state_hash());
    CB_CHECK(kraken_checksum(reference) == kraken_checksum(subject));
    CB_CHECK(reference.bids().size() == subject.bids().size());
    CB_CHECK(reference.asks().size() == subject.asks().size());

    std::vector<Level> a;
    std::vector<Level> b;
    for (const Side s : {Side::kBid, Side::kAsk}) {
        // Deeper than the checksum's 10 levels, so ordering errors below the
        // checksum horizon are still caught.
        const std::size_t na = reference.top(s, 32, a);
        const std::size_t nb = subject.top(s, 32, b);
        CB_CHECK(na == nb);
        CB_CHECK(a == b);

        Level la{};
        Level lb{};
        const bool ha = reference.best(s, la);
        const bool hb = subject.best(s, lb);
        CB_CHECK(ha == hb);
        CB_CHECK(!ha || la == lb);
    }
}

/// The same total oracle, one side container at a time.
///
/// ArraySide is a public container with its own contract over the whole int64
/// price range, and that range is where its window arithmetic wraps. The book
/// above it refuses negative prices, so driving the sides directly is the only
/// way to fuzz the half of the domain where the wrap lives.
void require_sides_equivalent(const MapSide& reference, const ArraySide& subject) {
    CB_CHECK(reference.size() == subject.size());

    Level la{};
    Level lb{};
    const bool ha = reference.best(la);
    const bool hb = subject.best(lb);
    CB_CHECK(ha == hb);
    CB_CHECK(!ha || la == lb);

    std::vector<Level> a;
    std::vector<Level> b;
    reference.for_each([&](const Level& lvl) {
        a.push_back(lvl);
        return a.size() < 64;
    });
    subject.for_each([&](const Level& lvl) {
        b.push_back(lvl);
        return b.size() < 64;
    });
    CB_CHECK(a == b);
}

void drive_sides(const std::uint8_t* data, std::size_t size) {
    MapSide ref_bid(Side::kBid);
    MapSide ref_ask(Side::kAsk);
    ArraySide sub_bid(Side::kBid);
    ArraySide sub_ask(Side::kAsk);
    Cursor cursor(data, size);

    while (!cursor.exhausted()) {
        const std::uint8_t op = cursor.byte();
        const bool bid = (op & 1) != 0;
        // Mix banded and extreme prices in the same stream: a wrap only shows up
        // when a far price meets a window that some near price anchored.
        const std::int64_t price = (op & 4) ? cursor.extreme_price() : cursor.price();
        const std::uint8_t qty_byte = cursor.byte();
        const std::int64_t qty = (op & 2) ? 0 : static_cast<std::int64_t>(qty_byte) + 1;

        if (bid) {
            ref_bid.apply(Price{price}, Qty{qty});
            sub_bid.apply(Price{price}, Qty{qty});
            require_sides_equivalent(ref_bid, sub_bid);
        } else {
            ref_ask.apply(Price{price}, Qty{qty});
            sub_ask.apply(Price{price}, Qty{qty});
            require_sides_equivalent(ref_ask, sub_ask);
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 4 || size > 65536) {
        return 0;
    }

    MapBook reference(InstrumentSpec{"BTC/USD", 1, 8});
    ArrayBook subject(InstrumentSpec{"BTC/USD", 1, 8});
    Cursor cursor(data, size);

    while (!cursor.exhausted()) {
        const std::uint8_t op = cursor.byte();

        // A rare clear exercises the resync path, where stale residue would
        // otherwise hide until the next snapshot.
        if ((op & 0x3F) == 0x3F) {
            reference.clear();
            subject.clear();
            require_equivalent(reference, subject);
            continue;
        }

        const Side side = (op & 1) ? Side::kBid : Side::kAsk;
        // One op bit selects the extreme generator. Without it the input domain
        // is a 65536-wide band around 452852 — exactly the window width, so no
        // two prices in it can be far enough apart to wrap the index maths.
        const std::int64_t price =
            (op & 4) ? cursor.nonnegative_extreme_price() : cursor.price();

        // Bias hard toward deletes. Removal is where the best-index hint has to
        // do real work, and it is the path most likely to be wrong.
        const std::uint8_t qty_byte = cursor.byte();
        const std::int64_t qty = (op & 2) ? 0 : static_cast<std::int64_t>(qty_byte) + 1;

        // Both books must agree on the verdict too: a rejected update has to be
        // rejected identically, or the "rejection" is just a divergence.
        const bool ok_ref = reference.apply(side, Price{price}, Qty{qty});
        const bool ok_sub = subject.apply(side, Price{price}, Qty{qty});
        CB_CHECK(ok_ref == ok_sub);

        require_equivalent(reference, subject);
    }

    // Then the same input again, straight into the side containers, over the
    // full signed range the book itself will not admit.
    drive_sides(data, size);

    return 0;
}
