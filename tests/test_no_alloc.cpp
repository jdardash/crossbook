// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// The no-allocation hot path, enforced rather than promised.
//
// "Zero allocations on the hot path" is a claim every low-latency README makes
// and almost none of them check. Here it is a test: global operator new is
// replaced with a counting version, armed around the critical section, and the
// test fails if the counter moves.
//
// That converts a marketing sentence into a build failure. If someone later
// adds a std::string to the update path, CI says so.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <vector>

#include "crossbook/book.hpp"
#include "crossbook/checksum.hpp"
#include "crossbook/sequence.hpp"

// ---------------------------------------------------------------------------
// Allocation probe
// ---------------------------------------------------------------------------

namespace alloc_probe {

std::atomic<bool> armed{false};
std::atomic<std::uint64_t> allocations{0};

/// RAII arm/disarm, so an assertion failure inside the guarded region cannot
/// leave the probe armed and poison every later test.
struct Guard {
    Guard() noexcept {
        allocations.store(0, std::memory_order_relaxed);
        armed.store(true, std::memory_order_relaxed);
    }
    ~Guard() { armed.store(false, std::memory_order_relaxed); }

    [[nodiscard]] static std::uint64_t count() noexcept {
        return allocations.load(std::memory_order_relaxed);
    }
};

inline void note() noexcept {
    if (armed.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void* raw_alloc(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) {
        bytes = 1;  // Distinct pointers for zero-size requests.
    }
#if defined(_MSC_VER)
    void* p = _aligned_malloc(bytes, alignment);
#else
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((bytes + alignment - 1) / alignment) * alignment;
    void* p = std::aligned_alloc(alignment, rounded);
#endif
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}

inline void raw_free(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

}  // namespace alloc_probe

// Replacing the global allocation functions is well-defined; every allocation
// in this translation unit's program image routes through here. All of them
// must be replaced together, or a mismatched pair frees a pointer the other
// allocator never handed out.

void* operator new(std::size_t n) {
    alloc_probe::note();
    return alloc_probe::raw_alloc(n, alignof(std::max_align_t));
}
void* operator new[](std::size_t n) {
    alloc_probe::note();
    return alloc_probe::raw_alloc(n, alignof(std::max_align_t));
}
void* operator new(std::size_t n, std::align_val_t a) {
    alloc_probe::note();
    return alloc_probe::raw_alloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    alloc_probe::note();
    return alloc_probe::raw_alloc(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note();
    try {
        return alloc_probe::raw_alloc(n, alignof(std::max_align_t));
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    alloc_probe::note();
    try {
        return alloc_probe::raw_alloc(n, alignof(std::max_align_t));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept { alloc_probe::raw_free(p); }
void operator delete[](void* p) noexcept { alloc_probe::raw_free(p); }
void operator delete(void* p, std::size_t) noexcept { alloc_probe::raw_free(p); }
void operator delete[](void* p, std::size_t) noexcept { alloc_probe::raw_free(p); }
void operator delete(void* p, std::align_val_t) noexcept { alloc_probe::raw_free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { alloc_probe::raw_free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::raw_free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { alloc_probe::raw_free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { alloc_probe::raw_free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { alloc_probe::raw_free(p); }

// ---------------------------------------------------------------------------

using namespace crossbook;

TEST_CASE("the allocation probe actually detects allocation", "[alloc]") {
    // A silently broken probe would make every test below pass while proving
    // nothing, so the detector is tested before it is trusted.
    std::uint64_t observed = 0;
    {
        alloc_probe::Guard guard;
        std::vector<int> v;
        v.reserve(1024);
        observed = alloc_probe::Guard::count();
    }
    CHECK(observed > 0);
}

TEST_CASE("ArrayBook updates do not allocate", "[alloc][book]") {
    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});

    // Warm-up outside the guard: the window buffer is allocated once at
    // construction, and the first update anchors it. Steady state is what the
    // claim is about.
    for (std::int64_t i = 0; i < 100; ++i) {
        book.apply(Side::kBid, Price{452852 - i}, Qty{i + 1});
        book.apply(Side::kAsk, Price{452853 + i}, Qty{i + 1});
    }

    std::uint64_t observed = 0;
    {
        alloc_probe::Guard guard;
        for (std::int64_t i = 0; i < 100'000; ++i) {
            const std::int64_t jitter = i % 200;
            book.apply(Side::kBid, Price{452852 - jitter}, Qty{(i % 997) + 1});
            book.apply(Side::kAsk, Price{452853 + jitter}, Qty{(i % 991) + 1});
            // Deletes exercise the removal path, which is where a naive
            // implementation reaches for a container operation that allocates.
            if (i % 8 == 0) {
                book.apply(Side::kBid, Price{452852 - jitter}, Qty{0});
            }
        }
        observed = alloc_probe::Guard::count();
    }
    CHECK(observed == 0);
}

TEST_CASE("a walking touch does not allocate", "[alloc][book]") {
    // THE TEST ABOVE PROVED LESS THAN IT LOOKED LIKE.
    //
    // It jitters around a FIXED touch, 200 ticks wide, inside a 65536-slot
    // window. So the book never leaves its initial window, the spill list is
    // never populated, and rebuild_around_touch() is never entered while the
    // probe is armed. Every allocation on the update path lived in exactly the
    // branch that test could not reach: a fresh `std::vector<Level>` with a
    // reserve() on every rebuild, plus a std::map node for every spilled level.
    // "No allocation on the hot path, enforced by a test" was true only of a
    // book whose touch does not move, which is not a book.
    //
    // Measured before the fix, BTCUSDT price_scale=2 over 20000 updates:
    // 12.5 ns/apply and 0 allocations at a 5000-tick live span (the widest
    // anything benchmarked), 30.6 us/apply and 84 allocations per update at
    // 40000 ticks, 773 us/apply and 5192 allocations per update at 1000000.
    //
    // So: walk the touch by several full windows, keeping a constant-depth book
    // by deleting from behind, which is what a live feed does over a session.
    // And assert rebuild_count() actually moved, because a no-allocation test
    // that never enters the allocating branch is the thing being replaced.
    constexpr std::int64_t kDepth = 300;
    constexpr std::int64_t kWarmSteps = 80'000;
    constexpr std::int64_t kMeasuredSteps = 200'000;  // > 3 windows of drift.

    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});

    std::int64_t ask_lo = 5'000'000;   // Asks occupy [ask_lo, ask_lo + kDepth).
    std::int64_t bid_lo = 20'000'000;  // Bids occupy [bid_lo, bid_lo + kDepth).
    for (std::int64_t i = 0; i < kDepth; ++i) {
        book.apply(Side::kAsk, Price{ask_lo + i}, Qty{i + 1});
        book.apply(Side::kBid, Price{bid_lo + i}, Qty{i + 1});
    }

    // Asks drift up, bids drift down: both touches leave their window, and the
    // bid path exercises the descending half of the rebuild's re-ordering.
    auto step = [&](std::int64_t n) {
        book.apply(Side::kAsk, Price{ask_lo + kDepth}, Qty{(n % 997) + 1});
        book.apply(Side::kAsk, Price{ask_lo}, Qty{0});
        ++ask_lo;
        book.apply(Side::kBid, Price{bid_lo - 1}, Qty{(n % 991) + 1});
        book.apply(Side::kBid, Price{bid_lo + kDepth - 1}, Qty{0});
        --bid_lo;
    };

    // Warm-up outside the guard. The scratch buffers are sized once for a book
    // of this depth and reused after that; steady state is what the claim is
    // about, exactly as for the fixed-touch test above.
    for (std::int64_t n = 0; n < kWarmSteps; ++n) {
        step(n);
    }
    const std::uint64_t ask_rebuilds_before = book.asks().rebuild_count();
    const std::uint64_t bid_rebuilds_before = book.bids().rebuild_count();
    REQUIRE(ask_rebuilds_before > 0);  // The warm-up must have re-anchored.

    std::uint64_t observed = 0;
    {
        alloc_probe::Guard guard;
        for (std::int64_t n = 0; n < kMeasuredSteps; ++n) {
            step(n);
        }
        observed = alloc_probe::Guard::count();
    }

    CHECK(observed == 0);

    // The probe only means something if the guarded region entered the branch
    // it is aimed at. Several re-anchors per side, on both the spill path and
    // the rebuild path.
    CHECK(book.asks().rebuild_count() > ask_rebuilds_before + 2);
    CHECK(book.bids().rebuild_count() > bid_rebuilds_before + 2);

    // And the book that came out the other side is still the right book: a
    // constant-depth ladder whose touch has moved by the full drift.
    CHECK_FALSE(book.asks().degraded());
    CHECK(book.asks().size() == static_cast<std::size_t>(kDepth));
    CHECK(book.bids().size() == static_cast<std::size_t>(kDepth));
    Level lvl{};
    REQUIRE(book.best(Side::kAsk, lvl));
    CHECK(lvl.price == Price{ask_lo});
    REQUIRE(book.best(Side::kBid, lvl));
    CHECK(lvl.price == Price{bid_lo + kDepth - 1});
}

TEST_CASE("checksum computation does not allocate", "[alloc][checksum]") {
    // This is why the verifier can run on every update rather than being
    // sampled: mantissa digits go into a stack buffer and straight into the
    // CRC, with no intermediate string.
    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < 50; ++i) {
        book.apply(Side::kBid, Price{452852 - i}, Qty{100000 + i});
        book.apply(Side::kAsk, Price{452853 + i}, Qty{100000 + i});
    }

    std::uint64_t observed = 0;
    std::uint32_t sink = 0;
    {
        alloc_probe::Guard guard;
        for (int i = 0; i < 10'000; ++i) {
            sink ^= kraken_checksum(book);
        }
        observed = alloc_probe::Guard::count();
    }
    CHECK(observed == 0);
    CHECK(sink != 0xFFFFFFFFU);  // Keep the loop from being optimised away.
}

TEST_CASE("book queries do not allocate", "[alloc][book]") {
    ArrayBook book(InstrumentSpec{"BTC/USD", 1, 8});
    for (std::int64_t i = 0; i < 50; ++i) {
        book.apply(Side::kBid, Price{452852 - i}, Qty{100000 + i});
        book.apply(Side::kAsk, Price{452853 + i}, Qty{100000 + i});
    }

    std::uint64_t observed = 0;
    std::uint64_t sink = 0;
    {
        alloc_probe::Guard guard;
        Level lvl{};
        for (int i = 0; i < 10'000; ++i) {
            if (book.best(Side::kBid, lvl)) {
                sink += static_cast<std::uint64_t>(lvl.price.ticks);
            }
            if (book.best(Side::kAsk, lvl)) {
                sink += static_cast<std::uint64_t>(lvl.price.ticks);
            }
            sink ^= book.state_hash();
        }
        observed = alloc_probe::Guard::count();
    }
    CHECK(observed == 0);
    CHECK(sink != 0);
}

TEST_CASE("sequence tracking does not allocate", "[alloc][sequence]") {
    // The gap detector runs on every message, so it belongs to the hot path
    // just as much as the book does.
    SequenceTracker tracker(SequencePolicy::kBinanceSpot);
    tracker.on_snapshot(1000);

    std::uint64_t observed = 0;
    {
        alloc_probe::Guard guard;
        // Start one below the snapshot id so the first event straddles it
        // (U <= lastUpdateId <= u), as Binance's procedure requires. Starting
        // above it is a resync, not a stream.
        SequenceId last = 999;
        for (int i = 0; i < 100'000; ++i) {
            const UpdateIds ids{last + 1, last + 3, last};
            (void)tracker.on_update(ids);
            last += 3;
        }
        observed = alloc_probe::Guard::count();
    }
    CHECK(observed == 0);
    CHECK(tracker.stats().applied == 100'000);
}
