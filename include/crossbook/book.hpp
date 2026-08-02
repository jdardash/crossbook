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
//               follows the book, with a sorted spill list for levels outside
//               it. Faster; the interesting one; the one that could be wrong.
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
#include <limits>
#include <map>
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
/// window base, with a sorted spill list holding anything outside it.
///
/// The bet: in a liquid book almost every update lands within a narrow band
/// around the touch, so the common case becomes an array store at a computed
/// index — no comparisons, no pointer chasing, no rebalancing. The spill list
/// exists only to keep far-from-touch levels correct, and it is expected to
/// stay nearly empty.
///
/// The window re-anchors when the spill list grows past a threshold, which is
/// the part most likely to harbour a bug and therefore the part the
/// differential test hammers hardest.
class ArraySide {
public:
    /// 65536 slots covers roughly a $6.5k band on BTC/USD at a 0.1 tick — far
    /// wider than the touch ever travels between re-anchors, while costing
    /// 512 KiB per side.
    static constexpr std::size_t kDefaultSlots = 1U << 16;

    /// Re-anchor once this many levels have spilled out of the window. Small
    /// enough that the spill list never dominates lookups; large enough that a
    /// brief excursion does not trigger a rebuild.
    ///
    /// Note this is a REBUILD TRIGGER, not a bound on the spill list. A book
    /// whose live price range is genuinely wider than the window leaves levels
    /// outside it no matter where the window is anchored, and the list stays
    /// above the threshold permanently; see `degraded()`.
    static constexpr std::size_t kOverflowRebuildThreshold = 64;

    /// Spill capacity reserved up front. One more than the rebuild threshold,
    /// which is the largest the list can reach before a rebuild fires while the
    /// window is healthy — so a healthy book never allocates here, and the
    /// allocation probe in tests/test_no_alloc.cpp can say so.
    static constexpr std::size_t kOverflowReserve = kOverflowRebuildThreshold + 1;

    explicit ArraySide(Side side, std::size_t slots = kDefaultSlots)
        : side_(side), slots_(slots), window_(slots, 0) {
        overflow_.reserve(kOverflowReserve);
    }

    void apply(Price price, Qty qty) {
        const std::int64_t ticks = price.ticks;

        if (!anchored_) {
            // First level seen defines the window, centred so the book can grow
            // in either direction before a rebuild is needed.
            if (qty.is_zero()) {
                return;  // Removing a level from an empty book is a no-op.
            }
            base_ = anchor_for(ticks);
            anchored_ = true;
        }

        std::size_t index = 0;
        if (window_index(ticks, index)) {
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
                mark_dirty(index);
            }
            return;
        }

        // Outside the window.
        if (qty.is_zero()) {
            overflow_erase(ticks);
            // A shrinking spill list lowers the floor the rebuild trigger sits
            // on, so a book that recovers from a wide excursion becomes
            // eligible for a real re-anchor again instead of staying latched.
            if (overflow_.size() < overflow_floor_) {
                overflow_floor_ = overflow_.size();
            }
            if (overflow_.empty()) {
                degraded_ = false;
            }
            return;
        }
        overflow_set(ticks, qty.units);

        // HYSTERESIS, AND WHY IT IS NOT OPTIONAL.
        //
        // The trigger used to be `overflow_.size() > kOverflowRebuildThreshold`
        // against an implicit floor of zero. When the live price range is wider
        // than the window, the rebuild cannot get the spill list back under the
        // threshold — the levels are outside every possible window — so the
        // very next out-of-window insert tripped the trigger again, and every
        // update from then on paid a full O(n) collect-and-refill. Measured on
        // BTCUSDT at price_scale=2 over 20000 updates: 12.5 ns/apply at a
        // 5000-tick live span, 30.6 us/apply at 40000 ticks, 773 us/apply at
        // 1000000 ticks, against a flat ~200 ns for MapBook. The array book
        // became four thousand times slower than the tree it exists to beat.
        //
        // So the trigger is measured against the floor the last rebuild left
        // behind: another full threshold of NEW spill has to accumulate before
        // it is worth re-anchoring again. That keeps the rebuild amortised at
        // O(n) per threshold of drift in every regime, and `degraded()` reports
        // that the window has stopped being able to contain the book rather
        // than leaving it to be inferred from a latency graph.
        if (overflow_.size() > overflow_floor_ + kOverflowRebuildThreshold) {
            rebuild_around_touch();
        }
    }

    void clear() noexcept {
        // Only the slots that were actually written get cleared. The full fill
        // is 512 KiB per side, ~52 us for the pair, and it evicts L2 — and
        // feed.hpp calls clear() on EVERY resync, which is precisely the moment
        // the book is behind the venue and racing to catch up. A live book
        // occupies a few thousand slots, so the watermarks turn a fixed 1 MiB
        // memset into a few tens of microseconds' worth of nothing.
        clear_window_slots();
        overflow_.clear();
        // Capacity is deliberately retained on both scratch buffers: a resync
        // is exactly when re-allocating them would hurt most.
        rebuild_scratch_.clear();
        window_count_ = 0;
        best_index_ = 0;
        anchored_ = false;
        base_ = 0;
        overflow_floor_ = 0;
        degraded_ = false;
        // rebuild_count_ is a lifetime counter, not per-snapshot state. Resetting
        // it here would hide rebuild thrash behind a busy resync loop.
    }

    [[nodiscard]] std::size_t size() const noexcept { return window_count_ + overflow_.size(); }
    [[nodiscard]] Side side() const noexcept { return side_; }

    /// Diagnostic: how many levels currently sit outside the window. Exposed so
    /// benchmarks and tests can assert the window is actually doing its job
    /// rather than silently degrading into a std::map with extra steps.
    [[nodiscard]] std::size_t overflow_size() const noexcept { return overflow_.size(); }

    /// Diagnostic: has re-anchoring stopped helping?
    ///
    /// Set when a rebuild fails to shrink the spill list, which means the live
    /// price range is wider than the window and no anchor can contain it. The
    /// book stays correct — this is a performance mode, not an error — but the
    /// O(1) window store degrades to a sorted-list insert for the spilled tail,
    /// and the no-allocation guarantee holds only up to `kOverflowReserve`
    /// levels. Exposed because a silent performance cliff is how a book that is
    /// "fast" in the benchmark becomes the slowest thing in production.
    [[nodiscard]] bool degraded() const noexcept { return degraded_; }

    /// Diagnostic: how many full re-anchors have happened over this side's
    /// lifetime. A rebuild is the only O(n) operation on the update path, so
    /// this is the number a test asserts against to prove the hysteresis above
    /// is real rather than described.
    [[nodiscard]] std::uint64_t rebuild_count() const noexcept { return rebuild_count_; }

    /// The level at the touch, if any.
    ///
    /// Dedicated rather than a one-level `for_each`, because the generic merge
    /// has to set up cursors for the spill side even when the spill list is
    /// empty — which is the overwhelmingly common case, and which benchmarked
    /// at 6x the cost of the equivalent std::map lookup. Reading the touch
    /// happens on every quote decision, so it earns a fast path.
    [[nodiscard]] bool best(Level& out) const noexcept {
        const bool have_window = (anchored_ && window_count_ > 0);
        if (!have_window) {
            if (overflow_.empty()) {
                return false;
            }
            out = overflow_touch();
            return true;
        }

        const std::int64_t win_price = price_at(best_index_);
        const std::int64_t win_qty = window_[best_index_];

        if (overflow_.empty()) {  // The fast path this exists for.
            out = Level{Price{win_price}, Qty{win_qty}};
            return true;
        }

        const Level spill = overflow_touch();
        if (precedes(side_, win_price, spill.price.ticks)) {
            out = Level{Price{win_price}, Qty{win_qty}};
        } else {
            out = spill;
        }
        return true;
    }

    /// Visit levels in book order until `fn` returns false.
    ///
    /// A two-way merge between the window scan and the spill list. Both are
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

        // The spill list is kept sorted ascending by price, so book order is a
        // forward walk for asks and a backward walk for bids.
        const std::size_t ov_n = overflow_.size();
        std::size_t ov_taken = 0;

        auto overflow_exhausted = [&]() { return ov_taken >= ov_n; };
        auto overflow_level = [&]() -> const Level& {
            return ascending ? overflow_[ov_taken] : overflow_[ov_n - 1 - ov_taken];
        };
        auto overflow_advance = [&]() { ++ov_taken; };

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
                take_window = precedes(side_, price_at(static_cast<std::size_t>(win_index)),
                                       overflow_level().price.ticks);
            }

            if (take_window) {
                const auto index = static_cast<std::size_t>(win_index);
                if (!fn(Level{Price{price_at(index)}, Qty{window_[index]}})) {
                    return;
                }
                ++emitted;
                win_index += step;
                seek_occupied();
            } else {
                if (!fn(overflow_level())) {
                    return;
                }
                overflow_advance();
            }
        }
    }

private:
    static constexpr std::size_t kNoDirty = std::numeric_limits<std::size_t>::max();

    /// Where to put the window base so `touch` sits in the middle of it, and so
    /// that [base_, base_ + slots_) stays entirely inside the int64 range.
    ///
    /// THE CLAMP IS THE WHOLE POINT, AND ITS ABSENCE WAS A SILENT CORRUPTION.
    ///
    /// `ticks - slots_/2` overflows for a touch near INT64_MIN, which is UB on
    /// its own. The worse half was what the old code then did with the result:
    /// membership was tested as `offset >= 0 && offset < slots_` on a signed
    /// `ticks - base_` that had ALSO wrapped, so a price at the opposite end of
    /// the int64 range came out with a small non-negative offset and was filed
    /// into the window as though it sat a few ticks from the touch. `for_each`
    /// emits window slots in index order, which after such a wrap is no longer
    /// price order, so the book reported its levels misordered and `best()`
    /// returned a level that was not the touch — with `overflow_size() == 0`,
    /// i.e. with no diagnostic pointing anywhere near the real cause.
    ///
    /// Concretely, on a default 65536-slot ask side: asks at INT64_MAX-10,
    /// INT64_MIN+5 and INT64_MAX-9 made ArraySide report a best ask of
    /// INT64_MAX-9 where MapSide reported INT64_MIN+5, and state_hash and the
    /// Kraken checksum disagreed with it.
    ///
    /// This is reachable from the wire, not theoretical: parse_fixed accepts
    /// "9223372036854775807" and both venue decoders hand the result straight
    /// into Price{} with no range check.
    ///
    /// Confining the window is also what makes the unsigned membership test in
    /// `window_index` EXACT rather than merely wrap-free. With the window
    /// entirely inside the int64 range, no out-of-window price can produce an
    /// unsigned offset below `slots_`; without the clamp, INT64_MIN+5 against a
    /// base of INT64_MAX-32778 produces the unsigned offset 32784, and unsigned
    /// arithmetic would have accepted the same wrong answer without the UB.
    [[nodiscard]] std::int64_t anchor_for(std::int64_t touch) const noexcept {
        constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
        constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
        const auto half = static_cast<std::int64_t>(slots_ / 2);
        // Width of the window minus one, saturated: a degenerate zero-slot side
        // never puts anything in the window, but must not compute a bogus bound.
        const std::int64_t span = (slots_ == 0) ? 0 : static_cast<std::int64_t>(slots_ - 1);
        const std::int64_t highest = kMax - span;

        if (touch < kMin + half) {
            return kMin;  // Centring would underflow; sit on the floor instead.
        }
        const std::int64_t centred = touch - half;
        return (centred > highest) ? highest : centred;
    }

    /// Is `ticks` inside the window, and at which slot?
    ///
    /// Unsigned because that is the only arithmetic here that can neither trap
    /// nor wrap into a false positive. `ticks - base_` in int64 is UB for prices
    /// roughly 2^63 away from the base; the same subtraction in uint64 is
    /// defined, and `offset < slots_` is exactly the in-window predicate — a
    /// price below the base wraps to something enormous, which fails the test
    /// for free, so there is no separate negative branch to get wrong.
    [[nodiscard]] bool window_index(std::int64_t ticks, std::size_t& index) const noexcept {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(ticks) - static_cast<std::uint64_t>(base_);
        if (offset >= static_cast<std::uint64_t>(slots_)) {
            return false;
        }
        index = static_cast<std::size_t>(offset);
        return true;
    }

    /// Price of a window slot. Also done in unsigned space: `anchor_for` keeps
    /// the whole window representable, so this never actually wraps, but doing
    /// the addition in int64 would still be UB by the letter of the standard for
    /// a base near INT64_MAX and is not worth arguing about at 3am.
    [[nodiscard]] std::int64_t price_at(std::size_t index) const noexcept {
        return static_cast<std::int64_t>(static_cast<std::uint64_t>(base_) +
                                         static_cast<std::uint64_t>(index));
    }

    /// Is window slot `candidate` closer to the touch than `incumbent`?
    /// Asks improve toward lower indices, bids toward higher ones.
    [[nodiscard]] bool better_index(std::size_t candidate, std::size_t incumbent) const noexcept {
        return side_ == Side::kAsk ? (candidate < incumbent) : (candidate > incumbent);
    }

    /// Widen the written-slot watermarks. Only ever widened, never narrowed on
    /// removal: narrowing correctly would need a reverse scan on every delete,
    /// and the span is bounded by the rebuild anyway.
    void mark_dirty(std::size_t index) noexcept {
        if (dirty_lo_ > dirty_hi_) {  // Nothing written since the last clear.
            dirty_lo_ = index;
            dirty_hi_ = index;
            return;
        }
        if (index < dirty_lo_) {
            dirty_lo_ = index;
        }
        if (index > dirty_hi_) {
            dirty_hi_ = index;
        }
    }

    void clear_window_slots() noexcept {
        if (dirty_lo_ <= dirty_hi_) {
            const auto lo = static_cast<std::ptrdiff_t>(dirty_lo_);
            const auto hi = static_cast<std::ptrdiff_t>(dirty_hi_);
            std::fill(window_.begin() + lo, window_.begin() + hi + 1, std::int64_t{0});
        }
        dirty_lo_ = kNoDirty;
        dirty_hi_ = 0;
    }

    // -- spill list -------------------------------------------------------
    //
    // A sorted vector, not a std::map. The list is small by construction (the
    // rebuild trigger keeps it within a threshold of its irreducible floor), so
    // the memmove of an ordered insert is cheaper than a red-black rebalance —
    // and, decisively, a std::map allocates a node on every single insert. That
    // allocation sat squarely on the update path while the README claimed the
    // hot path never allocates, and tests/test_no_alloc.cpp could not see it
    // because its generator never left the window.

    /// Index of the first spilled level at or after `ticks` — i.e. where it
    /// lives, or where it would be inserted. An index rather than an iterator
    /// so callers can mutate without juggling const_iterator conversions.
    [[nodiscard]] std::size_t overflow_lower_bound(std::int64_t ticks) const noexcept {
        const auto pos = std::lower_bound(
            overflow_.cbegin(), overflow_.cend(), ticks,
            [](const Level& lvl, std::int64_t t) noexcept { return lvl.price.ticks < t; });
        return static_cast<std::size_t>(std::distance(overflow_.cbegin(), pos));
    }

    void overflow_set(std::int64_t ticks, std::int64_t units) {
        const std::size_t index = overflow_lower_bound(ticks);
        if (index < overflow_.size() && overflow_[index].price.ticks == ticks) {
            overflow_[index].qty.units = units;
            return;
        }
        overflow_.insert(overflow_.begin() + static_cast<std::ptrdiff_t>(index),
                         Level{Price{ticks}, Qty{units}});
    }

    void overflow_erase(std::int64_t ticks) {
        const std::size_t index = overflow_lower_bound(ticks);
        if (index < overflow_.size() && overflow_[index].price.ticks == ticks) {
            overflow_.erase(overflow_.begin() + static_cast<std::ptrdiff_t>(index));
        }
    }

    /// Best spilled level in book order. Ascending storage means the touch is
    /// the front for asks and the back for bids.
    [[nodiscard]] Level overflow_touch() const noexcept {
        return (side_ == Side::kAsk) ? overflow_.front() : overflow_.back();
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

    /// Recentre the window on the current best price and fold the spill list
    /// back in wherever it now fits.
    ///
    /// The collect buffer is a member rather than a local. As a local it was a
    /// fresh `std::vector<Level>` with a `reserve(size())` on every rebuild —
    /// an allocation on the update path, in the one function on the update path
    /// that is already the most expensive thing here.
    void rebuild_around_touch() {
        ++rebuild_count_;
        const std::size_t before = overflow_.size();

        // Collect everything, since the window base is about to move.
        rebuild_scratch_.clear();
        rebuild_scratch_.reserve(size());
        for_each([&](const Level& lvl) {
            rebuild_scratch_.push_back(lvl);
            return true;
        });
        if (rebuild_scratch_.empty()) {
            return;
        }

        // rebuild_scratch_[0] is the touch, by construction of for_each.
        const std::int64_t touch = rebuild_scratch_.front().price.ticks;

        clear_window_slots();
        overflow_.clear();
        window_count_ = 0;
        best_index_ = 0;

        // Centre on the touch. A book grows away from the touch on one side
        // only, but quotes cross it constantly, so centring beats aligning the
        // edge and costs nothing.
        base_ = anchor_for(touch);
        anchored_ = true;

        for (const Level& lvl : rebuild_scratch_) {
            std::size_t index = 0;
            if (window_index(lvl.price.ticks, index)) {
                // `rebuild_scratch_` arrives in book order, so the first level
                // to land in the window is the best one in it.
                if (window_count_ == 0) {
                    best_index_ = index;
                }
                window_[index] = lvl.qty.units;
                mark_dirty(index);
                ++window_count_;
            } else {
                overflow_.push_back(lvl);
            }
        }
        // Appended in book order, and the spill list is keyed ascending. For
        // asks those coincide; for bids the appended run is exactly descending,
        // so one reverse restores the invariant without a sort.
        if (side_ == Side::kBid) {
            std::reverse(overflow_.begin(), overflow_.end());
        }

        const std::size_t after = overflow_.size();
        overflow_floor_ = after;
        // Re-anchoring bought nothing: the live range is wider than the window,
        // and repeating this on the next insert would be pure thrash.
        degraded_ = (after > 0 && after >= before);
    }

    Side side_;
    std::size_t slots_;
    std::vector<std::int64_t> window_;
    /// Levels outside the window, sorted ascending by price.
    std::vector<Level> overflow_;
    /// Reusable collect buffer for rebuild_around_touch(); keeps its capacity.
    std::vector<Level> rebuild_scratch_;
    std::int64_t base_{0};
    std::size_t window_count_{0};
    /// Index of the occupied slot nearest the touch. Meaningful only while
    /// `window_count_ > 0`. This is what makes reads O(1) instead of O(slots).
    std::size_t best_index_{0};
    /// Inclusive range of window slots that have been written since the last
    /// clear. `dirty_lo_ > dirty_hi_` means "none". This is what keeps clear()
    /// off the full 512 KiB.
    std::size_t dirty_lo_{kNoDirty};
    std::size_t dirty_hi_{0};
    /// Spill count left behind by the last rebuild — the irreducible tail that
    /// no anchor can absorb. The rebuild trigger is measured against this.
    std::size_t overflow_floor_{0};
    std::uint64_t rebuild_count_{0};
    bool degraded_{false};
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
    ///
    /// Returns false, and changes nothing at all, when the update is not a
    /// representable book level: a negative price or a negative quantity.
    ///
    /// WHY THIS IS A GUARD AND NOT A COMMENT.
    ///
    /// A negative quantity used to be stored as a live level, because removal
    /// keys on `is_zero()` rather than on sign. The checksum then hid it: the
    /// Kraken payload is the mantissa's DECIMAL DIGITS, and
    /// checksum.hpp::write_digits takes the magnitude, so a level of -5 emitted
    /// byte-identical bytes to a level of +5 and produced the same CRC32. A
    /// decoder sign bug therefore yielded a book that was wrong about resting
    /// size while still agreeing with the venue's own checksum — the precise
    /// failure this library exists to make impossible, dressed up as a pass.
    /// The same magnitude argument applies to price, and a negative price is not
    /// a thing on any venue covered here.
    ///
    /// REJECT MEANS DROP THE UPDATE AND SAY SO. The two alternatives were both
    /// worse. Storing it keeps the corruption. Treating it as a removal
    /// fabricates a market event the venue never sent — deleting a level that,
    /// for all we know, is still resting — and does it silently. Leaving the
    /// book untouched is the only option that cannot invent state, and the bool
    /// is the error channel the book otherwise lacks.
    ///
    /// CONTRACT FOR CALLERS: a false return means the decoder produced a level
    /// the book refused, which is a bug in the decoder or corruption on the
    /// wire, and it should be counted and logged like any other divergence
    /// rather than discarded. Deliberately NOT [[nodiscard]] — several call
    /// sites predate the return value and ignoring it must stay a decision, not
    /// a build break — but a caller that ignores it is choosing not to know.
    bool apply(Side s, Price price, Qty qty) {
        if (price.ticks < 0 || qty.units < 0) {
            return false;
        }
        side(s).apply(price, qty);
        return true;
    }

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
    ///
    /// `n` is caller-controlled and is bounded against the actual depth before
    /// it reaches reserve(). It used to be passed through raw, so
    /// `top(side, SIZE_MAX, out)` threw std::length_error out of what reads like
    /// a plain accessor — a read of the book taking down the process because of
    /// an argument that means "give me everything".
    [[nodiscard]] std::size_t top(Side s, std::size_t n, std::vector<Level>& out) const {
        out.clear();
        out.reserve(std::min(n, side(s).size()));
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
