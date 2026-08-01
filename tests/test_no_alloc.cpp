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
