// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Josh Dardashti
//
// L2 (price-aggregated) order book, with two interchangeable side containers.
//
// TWO IMPLEMENTATIONS, ON PURPOSE:
//
//   MapSide   — std::map. Obviously correct, easy to read, slow. This is the
//               reference implementation and the differential-test oracle.
//   ArraySide — tick-indexed direct-address array over a price window that
//               follows the book, with a std::map for levels outside it.
//               Faster; the interesting one; the one that could be wrong.
//
// Keeping both is not indecision. `tests/test_book_equivalence.cpp` drives
// identical event streams through both and asserts their full state matches
// after every single update, and the fuzzer does the same with generated input.
// A claim that the array beats the tree is only worth making if the array is
// also provably identical to it, and "provably" here means a test that fails
// when it isn't.
//
// Both containers hold quantities as ABSOLUTE values, matching every venue
// covered so far: an update carries the new resting size at that price, not a
// delta, and a size of zero removes the level.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include "crossbook/types.hpp"

namespace crossbook {

/// Does `a` come before `b` in book order for this side?
/// Bids run high to low, asks low to high.
[[nodiscard]] constexpr bool precedes(Side side, std::int64_t a, std::int64_t b) noexcept {
    return side == Side::kBid ? (a > b) : (a < b);
}

// ---------------------------------------------------------------------------
// MapSide — the reference
// ---------------------------------------------------------------------------

/// Price levels in a std::map, always keyed ascending; book order comes from
/// the iteration direction. Deliberately the dumbest thing that works.
class MapSide {
public:
    explicit MapSide(Side side) noexcept : side_(side) {}

    void apply(Price price, Qty qty) {
        if (qty.is_zero()) {
            levels_.erase(price.ticks);
        } else {
            levels_[price.ticks] = qty.units;
        }
    }

    void clear() noexcept { levels_.clear(); }

    [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }
    [[nodiscard]] Side side() const noexcept { return side_; }

    /// The level at the touch, if any.
    [[nodiscard]] bool best(Level& out) const noexcept {
        if (levels_.empty()) {
            return false;
        }
        if (side_ == Side::kAsk) {
            const auto it = levels_.begin();
            out = Level{Price{it->first}, Qty{it->second}};
        } else {
            const auto it = levels_.rbegin();
            out = Level{Price{it->first}, Qty{it->second}};
        }
        return true;
    }

    /// Visit levels in book order until `fn` returns false.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        if (side_ == Side::kAsk) {
            for (auto it = levels_.begin(); it != levels_.end(); ++it) {
                if (!fn(Level{Price{it->first}, Qty{it->second}})) {
                    return;
                }
            }
        } else {
            for (auto it = levels_.rbegin(); it != levels_.rend(); ++it) {
                if (!fn(Level{Price{it->first}, Qty{it->second}})) {
                    return;
                }
            }
        }
    }

private:
    Side side_;
    std::map<std::int64_t, std::int64_t> levels_;
};

// ---------------------------------------------------------------------------
// ArraySide — the one under test
// ---------------------------------------------------------------------------

/// Price levels in a direct-address array indexed by tick offset from a moving
/// window base, with a std::map holding anything outside the window.
///
/// The bet: in a liquid book almost every update lands within a narrow band
/// around the touch, so the common case becomes an array store at a computed
/// index — no comparisons, no pointer chasing, no rebalancing. The map exists
/// only to keep far-from-touch levels correct, and it is expected to stay
/// nearly empty.
///
/// The window re-anchors when the map grows past a threshold, which is the part
/// most likely to harbour a bug and therefore the part the differential test
/// hammers hardest.
class ArraySide {
public:
    /// 65536 slots covers roughly a $6.5k band on BTC/USD at a 0.1 tick — far
    /// wider than the touch ever travels between re-anchors, while costing
    /// 512 KiB per side.
    static constexpr std::size_t kDefaultSlots = 1U << 16;

    /// Re-anchor once this many levels have spilled out of the window. Small
    /// enough that the map never dominates lookups; large enough that a brief
    /// excursion does not trigger a rebuild.
    static constexpr std::size_t kOverflowRebuildThreshold = 64;

    explicit ArraySide(Side side, std::size_t slots = kDefaultSlots)
        : side_(side), slots_(slots), window_(slots, 0) {}

    void apply(Price price, Qty qty) {
        const std::int64_t ticks = price.ticks;

        if (!anchored_) {
            // First level seen defines the window, centred so the book can grow
            // in either direction before a rebuild is needed.
            if (qty.is_zero()) {
                return;  // Removing a level from an empty book is a no-op.
            }
            base_ = ticks - static_cast<std::int64_t>(slots_ / 2);
            anchored_ = true;
        }

        const std::int64_t offset = ticks - base_;
        if (offset >= 0 && offset < static_cast<std::int64_t>(slots_)) {
            const auto index = static_cast<std::size_t>(offset);
            auto& slot = window_[index];
            if (qty.is_zero()) {
                if (slot != 0) {
                    slot = 0;
                    --window_count_;
                    if (index == best_index_) {
                        advance_best_after_removal();
                    }
                }
            } else {
                if (slot == 0) {
                    if (window_count_ == 0 || better_index(index, best_index_)) {
                        best_index_ = index;
                    }
                    ++window_count_;
                }
                slot = qty.units;
            }
            return;
        }

        // Outside the window.
        if (qty.is_zero()) {
            overflow_.erase(ticks);
            return;
        }
        overflow_[ticks] = qty.units;
        if (overflow_.size() > kOverflowRebuildThreshold) {
            rebuild_around_touch();
        }
    }

    void clear() noexcept {
        // Only the occupied slots need clearing, but tracking them costs more
        // than the memset for realistic occupancy, and clear() is not hot.
        std::fill(window_.begin(), window_.end(), 0);
        overflow_.clear();
        window_count_ = 0;
        best_index_ = 0;
        anchored_ = false;
        base_ = 0;
    }

    [[nodiscard]] std::size_t size() const noexcept { return window_count_ + overflow_.size(); }
    [[nodiscard]] Side side() const noexcept { return side_; }

    /// Diagnostic: how many levels currently sit outside the window. Exposed so
    /// benchmarks and tests can assert the window is actually doing its job
    /// rather than silently degrading into a std::map with extra steps.
    [[nodiscard]] std::size_t overflow_size() const noexcept { return overflow_.size(); }

    /// The level at the touch, if any.
    ///
    /// Dedicated rather than a one-level `for_each`, because the generic merge
    /// has to construct map iterators for the overflow side even when the
    /// overflow is empty — which is the overwhelmingly common case, and which
    /// benchmarked at 6x the cost of the equivalent std::map lookup. Reading
    /// the touch happens on every quote decision, so it earns a fast path.
    [[nodiscard]] bool best(Level& out) const noexcept {
        const bool have_window = (anchored_ && window_count_ > 0);
        if (!have_window) {
            if (overflow_.empty()) {
                return false;
            }
            const auto it = (side_ == Side::kAsk) ? overflow_.begin()
                                                  : std::prev(overflow_.end());
            out = Level{Price{it->first}, Qty{it->second}};
            return true;
        }

        const std::int64_t win_price = base_ + static_cast<std::int64_t>(best_index_);
        const std::int64_t win_qty = window_[best_index_];

        if (overflow_.empty()) {  // The fast path this exists for.
            out = Level{Price{win_price}, Qty{win_qty}};
            return true;
        }

        const auto it = (side_ == Side::kAsk) ? overflow_.begin() : std::prev(overflow_.end());
        if (precedes(side_, win_price, it->first)) {
            out = Level{Price{win_price}, Qty{win_qty}};
        } else {
            out = Level{Price{it->first}, Qty{it->second}};
        }
        return true;
    }

    /// Visit levels in book order until `fn` returns false.
    ///
    /// A two-way merge between the window scan and the overflow map. Both are
    /// individually in book order, so merging by price keeps the whole thing in
    /// book order without materialising anything.
    ///
    /// The window scan starts at `best_index_` and stops once `window_count_`
    /// levels have been emitted, so reading the touch costs O(1) and reading
    /// the top N costs O(N + gaps). Scanning from the window edge instead —
    /// which is what this did before it was benchmarked — costs O(slots) on
    /// every read: ~32k empty slots walked to answer "what is the best bid",
    /// measured at 67us against 4ns for the tree. Correct, and useless.
    /// The lesson is in bench/bench_book.cpp; the fix is the best-index hint.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        const bool ascending = (side_ == Side::kAsk);
        const std::int64_t step = ascending ? 1 : -1;
        const auto slot_count = static_cast<std::int64_t>(slots_);

        auto ov_it = overflow_.begin();
        auto ov_rit = overflow_.rbegin();
        const auto ov_end = overflow_.end();
        const auto ov_rend = overflow_.rend();

        auto overflow_exhausted = [&]() {
            return ascending ? (ov_it == ov_end) : (ov_rit == ov_rend);
        };
        auto overflow_price = [&]() { return ascending ? ov_it->first : ov_rit->first; };
        auto overflow_qty = [&]() { return ascending ? ov_it->second : ov_rit->second; };
        auto overflow_advance = [&]() {
            if (ascending) {
                ++ov_it;
            } else {
                ++ov_rit;
            }
        };

        std::size_t emitted = 0;
        std::int64_t win_index =
            (anchored_ && window_count_ > 0) ? static_cast<std::int64_t>(best_index_) : -1;

        // Skip to the next occupied slot. Bounded by the count of levels still
        // unemitted, so an empty tail is never walked.
        auto seek_occupied = [&]() {
            if (emitted >= window_count_) {
                win_index = -1;
                return;
            }
            while (win_index >= 0 && win_index < slot_count &&
                   window_[static_cast<std::size_t>(win_index)] == 0) {
                win_index += step;
            }
            if (win_index < 0 || win_index >= slot_count) {
                win_index = -1;
            }
        };

        if (win_index >= 0) {
            seek_occupied();
        }

        while (win_index >= 0 || !overflow_exhausted()) {
            bool take_window;
            if (win_index < 0) {
                take_window = false;
            } else if (overflow_exhausted()) {
                take_window = true;
            } else {
                take_window = precedes(side_, base_ + win_index, overflow_price());
            }

            if (take_window) {
                const std::int64_t price = base_ + win_index;
                const std::int64_t qty = window_[static_cast<std::size_t>(win_index)];
                if (!fn(Level{Price{price}, Qty{qty}})) {
                    return;
                }
                ++emitted;
                win_index += step;
                seek_occupied();
            } else {
                if (!fn(Level{Price{overflow_price()}, Qty{overflow_qty()}})) {
                    return;
                }
                overflow_advance();
            }
        }
    }

private:
    /// Is window slot `candidate` closer to the touch than `incumbent`?
    /// Asks improve toward lower indices, bids toward higher ones.
    [[nodiscard]] bool better_index(std::size_t candidate, std::size_t incumbent) const noexcept {
        return side_ == Side::kAsk ? (candidate < incumbent) : (candidate > incumbent);
    }

    /// The best level was just removed; walk inward to the next occupied slot.
    ///
    /// Amortised cheap because books are dense at the touch — the next level is
    /// usually one or two ticks away. The worst case is a book whose entire
    /// window empties, which is bounded by the scan below and only happens on a
    /// mass cancel, where a linear pass is not the expensive part.
    void advance_best_after_removal() noexcept {
        if (window_count_ == 0) {
            best_index_ = 0;
            return;
        }
        const std::int64_t step = (side_ == Side::kAsk) ? 1 : -1;
        const auto slot_count = static_cast<std::int64_t>(slots_);
        std::int64_t i = static_cast<std::int64_t>(best_index_) + step;
        while (i >= 0 && i < slot_count && window_[static_cast<std::size_t>(i)] == 0) {
            i += step;
        }
        // If nothing remains in that direction the count is stale only in the
        // sense that the survivors sit on the far side of the old best, which
        // cannot happen: best_index_ is by construction the extreme occupied
        // slot. Clamp defensively rather than leaving an out-of-range hint.
        best_index_ = (i >= 0 && i < slot_count) ? static_cast<std::size_t>(i) : 0;
    }

    /// Recentre the window on the current best price and fold the overflow map
    /// back in wherever it now fits.
    void rebuild_around_touch() {
        // Collect everything, since the window base is about to move.
        std::vector<Level> all;
        all.reserve(size());
        for_each([&](const Level& lvl) {
            all.push_back(lvl);
            return true;
        });
        if (all.empty()) {
            return;
        }

        // all[0] is the touch, by construction of for_each.
        const std::int64_t touch = all.front().price.ticks;

        std::fill(window_.begin(), window_.end(), 0);
        overflow_.clear();
        window_count_ = 0;
        best_index_ = 0;

        // Centre on the touch. A book grows away from the touch on one side
        // only, but quotes cross it constantly, so centring beats aligning the
        // edge and costs nothing.
        base_ = touch - static_cast<std::int64_t>(slots_ / 2);
        anchored_ = true;

        for (const Level& lvl : all) {
            const std::int64_t offset = lvl.price.ticks - base_;
            if (offset >= 0 && offset < static_cast<std::int64_t>(slots_)) {
                const auto index = static_cast<std::size_t>(offset);
                // `all` arrives in book order, so the first level to land in
                // the window is the best one in it.
                if (window_count_ == 0) {
                    best_index_ = index;
                }
                window_[index] = lvl.qty.units;
                ++window_count_;
            } else {
                overflow_[lvl.price.ticks] = lvl.qty.units;
            }
        }
    }

    Side side_;
    std::size_t slots_;
    std::vector<std::int64_t> window_;
    std::map<std::int64_t, std::int64_t> overflow_;
    std::int64_t base_{0};
    std::size_t window_count_{0};
    /// Index of the occupied slot nearest the touch. Meaningful only while
    /// `window_count_ > 0`. This is what makes reads O(1) instead of O(slots).
    std::size_t best_index_{0};
    bool anchored_{false};
};

// ---------------------------------------------------------------------------
// The book
// ---------------------------------------------------------------------------

/// A two-sided L2 book over whichever side container you pick.
template <typename SideImpl>
class BasicL2Book {
public:
    explicit BasicL2Book(InstrumentSpec spec)
        : spec_(std::move(spec)), bids_(Side::kBid), asks_(Side::kAsk) {}

    [[nodiscard]] const InstrumentSpec& spec() const noexcept { return spec_; }

    [[nodiscard]] SideImpl& side(Side s) noexcept { return s == Side::kBid ? bids_ : asks_; }
    [[nodiscard]] const SideImpl& side(Side s) const noexcept {
        return s == Side::kBid ? bids_ : asks_;
    }

    [[nodiscard]] SideImpl& bids() noexcept { return bids_; }
    [[nodiscard]] SideImpl& asks() noexcept { return asks_; }
    [[nodiscard]] const SideImpl& bids() const noexcept { return bids_; }
    [[nodiscard]] const SideImpl& asks() const noexcept { return asks_; }

    /// Apply one absolute price-level update. Zero quantity removes the level.
    void apply(Side s, Price price, Qty qty) { side(s).apply(price, qty); }

    /// Drop all state. Venues send a fresh snapshot after a sequence break, and
    /// applying it onto a stale book is how phantom levels are born.
    void clear() noexcept {
        bids_.clear();
        asks_.clear();
    }

    [[nodiscard]] Timestamp last_update() const noexcept { return last_update_; }
    void set_last_update(Timestamp ts) noexcept { last_update_ = ts; }

    /// Best bid / best ask, if the side has any levels.
    [[nodiscard]] bool best(Side s, Level& out) const noexcept { return side(s).best(out); }

    /// Drop levels beyond `max_levels` on one side, keeping those nearest the
    /// touch. Returns how many were removed. `max_levels == 0` does nothing.
    ///
    /// DEPTH-LIMITED SUBSCRIPTIONS NEED THIS, AND THE NEED IS NOT OBVIOUS.
    ///
    /// A venue serving a top-N book tells you when a level is cancelled, but not
    /// when a level falls out of the window because a better one arrived — from
    /// its point of view there is nothing to report, the level simply is not in
    /// the top N any more. A client that never trims therefore accumulates
    /// levels the venue stopped tracking long ago. They sit there harmlessly,
    /// below the depth the checksum covers, until enough removals near the touch
    /// let one back into the top 10 — and then the checksum fails, on an update
    /// that did nothing wrong, minutes after the actual divergence.
    ///
    /// Measured against Kraken BTC/USD at depth 10: without trimming, 4 of 298
    /// updates mismatched over one minute, and the book had grown to 20 bid
    /// levels for a 10-level subscription. With it, every update matched.
    ///
    /// Allocation-free: doomed prices are batched through a stack buffer, and
    /// the loop repeats if one pass could not name them all.
    std::size_t trim(Side s, std::size_t max_levels) {
        if (max_levels == 0) {
            return 0;
        }

        std::size_t removed = 0;
        for (;;) {
            // Collect before removing: mutating a side while iterating it is
            // undefined for the map, and confusing for the array.
            std::array<Price, 64> doomed{};
            std::size_t seen = 0;
            std::size_t count = 0;
            side(s).for_each([&](const Level& lvl) {
                if (seen++ < max_levels) {
                    return true;
                }
                doomed[count++] = lvl.price;
                return count < doomed.size();
            });

            if (count == 0) {
                return removed;
            }
            for (std::size_t i = 0; i < count; ++i) {
                side(s).apply(doomed[i], Qty{0});
            }
            removed += count;
        }
    }

    /// Trim both sides. The common case, since a depth is per subscription.
    std::size_t trim(std::size_t max_levels) {
        return trim(Side::kBid, max_levels) + trim(Side::kAsk, max_levels);
    }

    /// Copy the top `n` levels of a side, in book order. Returns how many were
    /// available (which may be fewer than `n`).
    [[nodiscard]] std::size_t top(Side s, std::size_t n, std::vector<Level>& out) const {
        out.clear();
        out.reserve(n);
        side(s).for_each([&](const Level& lvl) {
            out.push_back(lvl);
            return out.size() < n;
        });
        return out.size();
    }

    /// Order-sensitive hash of the entire book state.
    ///
    /// This is the determinism gate: replaying the same capture must produce
    /// the same hash, bit for bit, on every platform and every build. FNV-1a
    /// over the mantissas — no floats anywhere, so there is nothing platform-
    /// dependent left to disagree about.
    [[nodiscard]] std::uint64_t state_hash() const noexcept {
        std::uint64_t h = 1469598103934665603ULL;  // FNV-1a 64 offset basis
        auto mix = [&h](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) {
                h ^= (v >> (i * 8)) & 0xFFULL;
                h *= 1099511628211ULL;  // FNV-1a 64 prime
            }
        };
        for (const Side s : {Side::kBid, Side::kAsk}) {
            // Side tag, then level count, then a domain separator. Without the
            // count and the separator the tag is just another mantissa in the
            // stream, and a book with one bid at (1, 1) hashed identically to
            // a book with one ask at (1, 1) — a collision in the value the
            // determinism gate compares.
            mix(static_cast<std::uint64_t>(s));
            mix(static_cast<std::uint64_t>(side(s).size()));
            mix(0x5EA5'0FF5'C0DE'D00DULL);
            side(s).for_each([&](const Level& lvl) {
                mix(static_cast<std::uint64_t>(lvl.price.ticks));
                mix(static_cast<std::uint64_t>(lvl.qty.units));
                return true;
            });
        }
        return h;
    }

private:
    InstrumentSpec spec_;
    SideImpl bids_;
    SideImpl asks_;
    Timestamp last_update_{0};
};

/// The reference book: obviously correct, used as the differential oracle.
using MapBook = BasicL2Book<MapSide>;

/// The fast book: tick-indexed, proven equal to MapBook by test and by fuzzer.
using ArrayBook = BasicL2Book<ArraySide>;

}  // namespace crossbook
